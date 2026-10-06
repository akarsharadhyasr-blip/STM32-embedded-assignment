#include "adc_stream.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

extern ADC_HandleTypeDef  hadc1;    
extern UART_HandleTypeDef huart1;   

#define SAMPLE_MS   100u            
#define WIN         10              
#define ADC_VREF_MV 3300u
#define ADC_FULL    4095u           // 12-bit
static uint16_t ma_buf[WIN];
static uint8_t  ma_idx, ma_count;
static uint32_t ma_sum;

static uint16_t ma_update(uint16_t s) {
    if (ma_count == WIN) ma_sum -= ma_buf[ma_idx];  
    else ma_count++;                                
    ma_buf[ma_idx] = s;
    ma_sum += s;
    ma_idx = (ma_idx + 1) % WIN;
    return (uint16_t)((ma_sum + ma_count / 2) / ma_count);  
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
        if ((int32_t)(HAL_GetTick() - next) < 0) continue;
        next += SAMPLE_MS;

        uint16_t raw;
        if (!adc_read(&raw)) {
            const char *e = "adc error\r\n";
            HAL_UART_Transmit(&huart1, (uint8_t *)e, strlen(e), 100);
            continue;
        }

        uint16_t avg = ma_update(raw);
        uint32_t mv  = (uint32_t)avg * ADC_VREF_MV / ADC_FULL;  

        int n = snprintf(line, sizeof line, "t=%lu ms  raw=%u  avg=%u  (%lu mV)\r\n",
                         (unsigned long)HAL_GetTick(), raw, avg, (unsigned long)mv);

        
        if (n > 0 && n < (int)sizeof line &&
            HAL_UART_GetState(&huart1) != HAL_UART_STATE_BUSY_TX)
            HAL_UART_Transmit(&huart1, (uint8_t *)line, (uint16_t)n, 100);
    }
}
