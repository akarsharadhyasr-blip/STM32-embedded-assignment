#include "esp_modem.h"
#include "main.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern UART_HandleTypeDef huart1;   // ESP32
extern UART_HandleTypeDef huart2;   // console

#define HEARTBEAT_MS   30000u
#define TOPIC_HB       "stm32/heartbeat"
#define TOPIC_STATUS   "stm32/status"
#define TOPIC_CMD      "stm32/cmd"
#define CLIENT_ID      "stm32-host-01"

// --- rx ring buffers, filled from the uart interrupt ---
#define RB_SZ 256
typedef struct { volatile uint16_t head, tail; uint8_t buf[RB_SZ]; } ring_t;
static ring_t esp_rx, con_rx;
static uint8_t esp_b, con_b;

static void rb_put(ring_t *r, uint8_t c) {
    uint16_t n = (r->head + 1) % RB_SZ;
    if (n != r->tail) { r->buf[r->head] = c; r->head = n; }  // drop if full
}
static int rb_get(ring_t *r, uint8_t *c) {
    if (r->tail == r->head) return 0;
    *c = r->buf[r->tail]; r->tail = (r->tail + 1) % RB_SZ; return 1;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *h) {
    if (h == &huart1) { rb_put(&esp_rx, esp_b); HAL_UART_Receive_IT(&huart1, &esp_b, 1); }
    else if (h == &huart2) { rb_put(&con_rx, con_b); HAL_UART_Receive_IT(&huart2, &con_b, 1); }
}
void HAL_UART_ErrorCallback(UART_HandleTypeDef *h) {
    if (h == &huart1) HAL_UART_Receive_IT(&huart1, &esp_b, 1);
    else if (h == &huart2) HAL_UART_Receive_IT(&huart2, &con_b, 1);
}

static void esp_write(const char *s) { HAL_UART_Transmit(&huart1, (uint8_t *)s, strlen(s), 1000); }
static void con_write(const char *s) { HAL_UART_Transmit(&huart2, (uint8_t *)s, strlen(s), 1000); }

// creds typed in at runtime
static char cfg_ssid[33], cfg_pass[65], cfg_broker[64];
static uint16_t cfg_port = 1883;

static char line[160]; static uint16_t line_len;
static volatile int resp_ok, resp_err, prompt, wifi_up, mqtt_up;
static char msg_topic[64], msg_data[96]; static volatile int msg_pending;

static void capture_subrecv(const char *l) {
    const char *t0 = strchr(l, '"'); if (!t0) return; t0++;
    const char *t1 = strchr(t0, '"'); if (!t1) return;
    int tl = t1 - t0; if (tl > (int)sizeof msg_topic - 1) tl = sizeof msg_topic - 1;
    memcpy(msg_topic, t0, tl); msg_topic[tl] = 0;
    const char *p = strchr(t1, ','); if (!p) return; p++;
    while (*p >= '0' && *p <= '9') p++;
    if (*p == ',') p++;
    snprintf(msg_data, sizeof msg_data, "%s", p);
    msg_pending = 1;
}

static void classify(const char *l) {
    if (!l[0]) return;
    if (!strcmp(l, "OK"))                      resp_ok = 1;
    else if (!strcmp(l, "ERROR") || !strcmp(l, "FAIL")) resp_err = 1;
    else if (!strncmp(l, "+MQTTSUBRECV:", 13)) capture_subrecv(l);
    else if (!strncmp(l, "+MQTTCONNECTED", 14)) mqtt_up = 1;
    else if (!strncmp(l, "+MQTTDISCONNECTED", 17)) mqtt_up = 0;
    else if (strstr(l, "WIFI GOT IP"))         wifi_up = 1;
    else if (strstr(l, "WIFI DISCONNECT"))     wifi_up = 0;
}

// read whatever the esp sent and break it into lines
static void pump(void) {
    uint8_t c;
    while (rb_get(&esp_rx, &c)) {
        if (c == '>') { prompt = 1; continue; }   // pubraw data prompt
        if (c == '\r') continue;
        if (c == '\n') { line[line_len] = 0; classify(line); line_len = 0; continue; }
        if (line_len < sizeof line - 1) line[line_len++] = c;
    }
}

// send a command and wait for OK/ERROR/timeout (or '>' if want_prompt)
static int at_cmd(const char *cmd, uint32_t timeout, int want_prompt) {
    resp_ok = resp_err = prompt = 0;
    esp_write(cmd); esp_write("\r\n");
    uint32_t t0 = HAL_GetTick();
    for (;;) {
        pump();
        if (want_prompt && prompt) return 1;
        if (!want_prompt && resp_ok) return 1;
        if (resp_err) return 0;
        if (HAL_GetTick() - t0 > timeout) return 0;
    }
}

// pubraw avoids escaping the json commas/quotes
static int mqtt_pub(const char *topic, const char *json) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "AT+MQTTPUBRAW=0,\"%s\",%d,1,0", topic, (int)strlen(json));
    if (!at_cmd(cmd, 3000, 1)) return 0;
    esp_write(json);
    resp_ok = resp_err = 0;
    uint32_t t0 = HAL_GetTick();
    while (!resp_ok && !resp_err && HAL_GetTick() - t0 < 3000) pump();
    return resp_ok;
}

static void con_readline(char *dst, int max) {
    int n = 0; uint8_t c;
    for (;;) {
        if (!rb_get(&con_rx, &c)) continue;
        if (c == '\r' || c == '\n') { if (n == 0) continue; dst[n] = 0; con_write("\r\n"); return; }
        if ((c == 8 || c == 127) && n) { n--; con_write("\b \b"); continue; }
        if (n < max - 1) { dst[n++] = c; HAL_UART_Transmit(&huart2, &c, 1, 100); }
    }
}

// serial menu so wifi creds aren't hardcoded
static void provision(void) {
    char in[16];
    for (;;) {
        con_write("\r\n==== Wi-Fi / MQTT Provisioning ====\r\n");
        con_write(" 1) SSID   2) Password   3) Broker   4) Port   5) Connect\r\n> ");
        con_readline(in, sizeof in);
        switch (in[0]) {
        case '1': con_write("SSID> ");     con_readline(cfg_ssid,   sizeof cfg_ssid);   break;
        case '2': con_write("Password> "); con_readline(cfg_pass,   sizeof cfg_pass);   break;
        case '3': con_write("Broker> ");   con_readline(cfg_broker, sizeof cfg_broker); break;
        case '4': { char p[8]; con_write("Port> "); con_readline(p, sizeof p);
                    int v = atoi(p); if (v > 0 && v <= 65535) cfg_port = v; break; }
        case '5': if (cfg_ssid[0] && cfg_broker[0]) { con_write("Connecting...\r\n"); return; }
                  con_write("Set SSID and broker first\r\n"); break;
        default:  break;
        }
    }
}

// cwmode + cwjap, then mqtt connect + subscribe
static int bring_up(void) {
    char cmd[160];
    at_cmd("ATE0", 1000, 0);
    if (!at_cmd("AT", 1000, 0))              { con_write("[ERR] no modem\r\n");    return 0; }
    if (!at_cmd("AT+CWMODE=1", 2000, 0))     { con_write("[ERR] CWMODE\r\n");      return 0; }
    snprintf(cmd, sizeof cmd, "AT+CWJAP=\"%s\",\"%s\"", cfg_ssid, cfg_pass);
    con_write("[..] joining Wi-Fi\r\n");
    if (!at_cmd(cmd, 20000, 0))              { con_write("[ERR] CWJAP\r\n");       return 0; }
    con_write("[OK] Wi-Fi connected\r\n");
    snprintf(cmd, sizeof cmd, "AT+MQTTUSERCFG=0,1,\"%s\",\"\",\"\",0,0,\"\"", CLIENT_ID);
    if (!at_cmd(cmd, 3000, 0))               { con_write("[ERR] MQTT cfg\r\n");    return 0; }
    snprintf(cmd, sizeof cmd, "AT+MQTTCONN=0,\"%s\",%u,1", cfg_broker, cfg_port);
    if (!at_cmd(cmd, 10000, 0))              { con_write("[ERR] MQTT conn\r\n");   return 0; }
    con_write("[OK] MQTT connected\r\n");
    snprintf(cmd, sizeof cmd, "AT+MQTTSUB=0,\"%s\",1", TOPIC_CMD);
    if (!at_cmd(cmd, 5000, 0))               { con_write("[ERR] MQTT sub\r\n");    return 0; }
    con_write("[OK] subscribed\r\n");
    mqtt_pub(TOPIC_STATUS, "{\"state\":\"online\"}");
    return 1;
}

static void handle_command(const char *topic, const char *data) {
    char l[160]; snprintf(l, sizeof l, "[CMD] %s -> %s\r\n", topic, data); con_write(l);
    if (!strncmp(data, "ping", 4)) mqtt_pub(TOPIC_STATUS, "{\"ack\":\"pong\"}");
    else                           mqtt_pub(TOPIC_STATUS, "{\"ack\":\"unknown\"}");
}

void esp_modem_run(void) {
    HAL_UART_Receive_IT(&huart1, &esp_b, 1);
    HAL_UART_Receive_IT(&huart2, &con_b, 1);
    con_write("\r\n=== STM32 host <-> ESP32 AT modem ===\r\n");

    provision();
    while (!bring_up()) { con_write("[..] retry in 5 s\r\n"); HAL_Delay(5000); }

    uint32_t last_hb = HAL_GetTick() - HEARTBEAT_MS;   // send first one right away
    uint32_t seq = 0, boot = HAL_GetTick();

    for (;;) {
        pump();

        if (msg_pending) { msg_pending = 0; handle_command(msg_topic, msg_data); }

        if (HAL_GetTick() - last_hb >= HEARTBEAT_MS) {
            last_hb += HEARTBEAT_MS;
            char js[128];
            snprintf(js, sizeof js,
                "{\"dev\":\"%s\",\"seq\":%lu,\"uptime_s\":%lu,\"wifi\":%d,\"mqtt\":%d}",
                CLIENT_ID, (unsigned long)seq++,
                (unsigned long)((HAL_GetTick() - boot) / 1000), wifi_up, mqtt_up);
            if (mqtt_pub(TOPIC_HB, js)) { char l[160]; snprintf(l, sizeof l, "[HB] %s\r\n", js); con_write(l); }
        }

        if (!mqtt_up) {   // lost the link, redo the whole setup
            con_write("[..] link lost, reconnecting\r\n");
            while (!bring_up()) HAL_Delay(5000);
            last_hb = HAL_GetTick();
        }
    }
}
