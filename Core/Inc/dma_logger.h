#ifndef DMA_LOGGER_H
#define DMA_LOGGER_H

#include <stdbool.h>
#include <stdint.h>

#define DMA_LOGGER_BLOCK_SAMPLES 64U
#define DMA_LOGGER_DEFAULT_DECIMATION 100U

/* Main context only; restart only after IsBusy becomes false. */
void DmaLogger_Start(uint32_t decimation, uint32_t period_ns,
                     const float offsets[4], float amps_per_count);
void DmaLogger_Stop(void);
void DmaLogger_Task(void);
bool DmaLogger_IsBusy(void);
uint32_t DmaLogger_Overruns(void);
/* Single ADC ISR producer. No formatting, UART calls or waiting. */
void DmaLogger_Push(uint32_t sequence, uint16_t u1, uint16_t v,
                    uint16_t u2, uint16_t w, uint8_t sector);

#endif
