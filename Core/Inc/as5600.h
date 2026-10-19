#ifndef AS5600_H
#define AS5600_H
#include "stm32g4xx_hal.h"
#include <stdbool.h>

/* electrical_rad is uncalibrated; FOC applies direction/offset to mechanical_rad. */
typedef struct {
  uint16_t raw;
  float mechanical_rad, electrical_rad;
  uint32_t request_cycles, received_cycles, updated_ms, sequence;
  bool valid;
  /* Approximate reference: transaction start, not a sensor measurement latch.
   * Sensor internal/filter delay is NOT compensated by this timestamp. */
  uint32_t reference_cycles;
} AS5600_Sample;
void AS5600_Init(I2C_HandleTypeDef *i2c, TIM_HandleTypeDef *timer);
void AS5600_Task(void);
void AS5600_Tick(void); /* TIM1/ADC request path; rate-limited, never waits for data. */
bool AS5600_GetSample(AS5600_Sample *sample);
bool AS5600_ProcessCommand(const char *command);
void AS5600_Pause(void);
void AS5600_Resume(void);
#endif
