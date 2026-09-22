#ifndef BUS_VOLTAGE_H
#define BUS_VOLTAGE_H

#include "stm32g4xx_hal.h"
#include <stdbool.h>

/* ADC1 regular専用。呼び出し・参照はすべてmainコンテキスト。 */
typedef struct {
  float volts;
  float vref_volts;
  uint32_t updated_ms;
  uint16_t raw;
  bool valid;
  bool overvoltage;
} BusVoltageSample;

HAL_StatusTypeDef BusVoltage_Init(ADC_HandleTypeDef *adc);
void BusVoltage_Task(void);
bool BusVoltage_IsAcquiring(void);
bool BusVoltage_GetSample(BusVoltageSample *sample);
bool BusVoltage_ProcessCommand(const char *command);
void BusVoltage_Print(void);

#endif
