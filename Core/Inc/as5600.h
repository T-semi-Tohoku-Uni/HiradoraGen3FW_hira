#ifndef AS5600_H
#define AS5600_H
#include "stm32g4xx_hal.h"
#include <stdbool.h>

/* Angle acquisition only. No electrical angle or FOC timestamp is published. */
typedef struct {
  uint16_t raw;
  float mechanical_rad;
  uint32_t request_cycles, received_cycles, updated_ms, sequence;
  bool valid;
} AS5600_Sample;
void AS5600_Init(I2C_HandleTypeDef *i2c);
void AS5600_Task(void);
bool AS5600_GetSample(AS5600_Sample *sample);
bool AS5600_ProcessCommand(const char *command);
void AS5600_Pause(void);
void AS5600_Resume(void);
#endif
