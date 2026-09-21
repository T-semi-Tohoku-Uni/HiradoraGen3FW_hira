#ifndef NTC_H
#define NTC_H

#include "stm32g4xx_hal.h"
#include <stdbool.h>

/* Call after CurrentSense_Init() has calibrated ADC2. */
void NTC_Init(ADC_HandleTypeDef *adc);
/* Main-loop only. Pauses sampling/output during current logging and DMA drain. */
void NTC_Task(void);
/* "ntc" / "ntc start": stream; "ntc stop": stop streaming. */
bool NTC_ProcessCommand(const char *command);

#endif /* NTC_H */
