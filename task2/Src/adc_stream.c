#include "adc_stream.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

extern ADC_HandleTypeDef  hadc1;    // analog input (PA0 / ADC1_IN0)
extern UART_HandleTypeDef huart1;   // output (PA9 TX, 115200)

#define SAMPLE_MS   100u            // 100 ms -> 10 Hz
#define WIN         10              // moving-average window (samples)
#define ADC_VREF_MV 3300u
#define ADC_FULL    4095u           // 12-bit

// --- moving average over the last WIN samples ---
// keeps a running sum so each update is just one add + one subtract,
// instead of re-summing the whole buffer every time.
static uint16_t ma_buf[WIN];
static uint8_t  ma_idx, ma_count;
static uint32_t ma_sum;

static uint16_t ma_update(uint16_t s) {
    if (ma_count == WIN) ma_sum -= ma_buf[ma_idx];  // buffer full: drop oldest
    else ma_count++;                                // still filling up
    ma_buf[ma_idx] = s;
    ma_sum += s;
    ma_idx = (ma_idx + 1) % WIN;
    return (uint16_t)((ma_sum + ma_count / 2) / ma_count);  // rounded average
}

// one polled ADC conversion, with basic error checks
static int adc_read(uint16_t *out) {
    if (HAL_ADC_Start(&hadc1) != HAL_OK) return 0;
    if (HAL_ADC_PollForConversion(&hadc1, 10) != HAL_OK) { HAL_ADC_Stop(&hadc1); return 0; }
    *out = (uint16_t)HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);
    return 1;
}

void adc_stream_run(void) {
    char line[80];
    uint32_t next = HAL_GetTick();

    for (;;) {
        // fire every 100 ms. using += keeps the cadence steady even if a
        // transmit takes a little time, so we don't drift off 10 Hz.
        if ((int32_t)(HAL_GetTick() - next) < 0) continue;
        next += SAMPLE_MS;

        uint16_t raw;
        if (!adc_read(&raw)) {
            const char *e = "adc error\r\n";
            HAL_UART_Transmit(&huart1, (uint8_t *)e, strlen(e), 100);
            continue;
        }

        uint16_t avg = ma_update(raw);
        uint32_t mv  = (uint32_t)avg * ADC_VREF_MV / ADC_FULL;  // counts -> millivolts

        int n = snprintf(line, sizeof line, "t=%lu ms  raw=%u  avg=%u  (%lu mV)\r\n",
                         (unsigned long)HAL_GetTick(), raw, avg, (unsigned long)mv);

        // skip if the line didn't fit or the uart is mid-transmit
        if (n > 0 && n < (int)sizeof line &&
            HAL_UART_GetState(&huart1) != HAL_UART_STATE_BUSY_TX)
            HAL_UART_Transmit(&huart1, (uint8_t *)line, (uint16_t)n, 100);
    }
}
