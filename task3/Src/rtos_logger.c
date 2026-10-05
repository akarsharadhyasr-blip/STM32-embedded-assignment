#include "rtos_logger.h"
#include "main.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

extern UART_HandleTypeDef huart1;   // log output (PA9 TX, 115200)

#define BTN1_PIN     GPIO_PIN_0     // switch 1 -> PB0 / EXTI0
#define BTN2_PIN     GPIO_PIN_1     // switch 2 -> PB1 / EXTI1
#define DEBOUNCE_MS  200u
#define EVT_Q_LEN    8
#define LOG_Q_LEN    8
#define JSON_MAX     56

typedef struct { uint8_t id; uint32_t ts; } btn_evt_t;   // raw press (ISR -> producer)
typedef struct { char json[JSON_MAX]; }     log_msg_t;   // formatted line (producer -> consumer)

// the two synchronization boundaries:
static QueueHandle_t evt_queue;     // boundary 1: EXTI ISR  -> producer task
static QueueHandle_t log_queue;     // boundary 2: producer  -> consumer task
static volatile uint32_t dropped;   // events lost to a full queue

// ---- EXTI interrupt: the event source for the producer ----
// runs in interrupt context, so it must not block. just debounce, grab the
// button id + timestamp, and drop it on evt_queue. no formatting, no UART here.
void HAL_GPIO_EXTI_Callback(uint16_t pin) {
    static uint32_t last1, last2;   // per-button debounce
    uint32_t now = HAL_GetTick();
    btn_evt_t e;

    if (pin == BTN1_PIN)      { if (now - last1 < DEBOUNCE_MS) return; last1 = now; e.id = 1; }
    else if (pin == BTN2_PIN) { if (now - last2 < DEBOUNCE_MS) return; last2 = now; e.id = 2; }
    else return;
    e.ts = now;

    if (evt_queue == NULL) return;  // not up yet (press during boot)

    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(evt_queue, &e, &woken) != pdTRUE) dropped++;  // full -> drop
    portYIELD_FROM_ISR(woken);      // wake the producer right away if needed
}

// ---- PRODUCER task: raw event -> JSON -> log queue ----
static void producer_task(void *arg) {
    (void)arg;
    btn_evt_t e;
    log_msg_t m;
    for (;;) {
        // block until the ISR delivers a press (boundary 1)
        if (xQueueReceive(evt_queue, &e, portMAX_DELAY) == pdTRUE) {
            snprintf(m.json, sizeof m.json,
                     "{\"button_id\": %u, \"timestamp\": %lu}\r\n",
                     (unsigned)e.id, (unsigned long)e.ts);
            // pass the formatted line to the consumer (boundary 2). bounded wait
            // so a stuck consumer can't block the producer forever.
            if (xQueueSend(log_queue, &m, pdMS_TO_TICKS(100)) != pdTRUE) dropped++;
        }
    }
}

// ---- CONSUMER task: log queue -> UART ----
static void consumer_task(void *arg) {
    (void)arg;
    log_msg_t m;
    for (;;) {
        // block efficiently on the queue. the 1 s timeout avoids busy-waiting
        // on an empty queue and lets us report any dropped events.
        if (xQueueReceive(log_queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
            HAL_UART_Transmit(&huart1, (uint8_t *)m.json, (uint16_t)strlen(m.json), 100);
        } else if (dropped) {
            char w[32];
            int n = snprintf(w, sizeof w, "{\"dropped\": %lu}\r\n", (unsigned long)dropped);
            dropped = 0;
            if (n > 0) HAL_UART_Transmit(&huart1, (uint8_t *)w, (uint16_t)n, 100);
        }
    }
}

void logger_start(void) {
    evt_queue = xQueueCreate(EVT_Q_LEN, sizeof(btn_evt_t));
    log_queue = xQueueCreate(LOG_Q_LEN, sizeof(log_msg_t));
    configASSERT(evt_queue != NULL && log_queue != NULL);

    // producer as its own task; the consumer runs in the calling (default) task
    xTaskCreate(producer_task, "producer", 256, NULL, tskIDLE_PRIORITY + 2, NULL);
    consumer_task(NULL);            // never returns
}
