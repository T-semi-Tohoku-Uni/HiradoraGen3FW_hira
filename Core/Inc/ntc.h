#ifndef NTC_H
#define NTC_H

#include "stm32g4xx_hal.h"
#include <stdbool.h>

/* Call after CurrentSense_Init() has calibrated ADC2. */
void NTC_Init(ADC_HandleTypeDef *adc);
/* Main-loop only. Pauses sampling/output during current logging and DMA drain. */
void NTC_Task(void);
/* main専用。基板温度（モーター温度ではない）。100ms超の古い値は拒否。 */
bool NTC_GetTemperature(float *temperature_c);
/* "ntc" / "ntc start": stream; "ntc stop": stop streaming. */
bool NTC_ProcessCommand(const char *command);

#endif /* NTC_H */
