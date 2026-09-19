#ifndef AS5047P_H
#define AS5047P_H

#include "stm32g4xx_hal.h"
#include <stdbool.h>

/* Main-loop only. SPI must already be configured for Mode 1, 16-bit, MSB first.
 * Reporting is initially off; "angle" starts reporting at ~100 ms intervals.
 * Waits 10 ms, then prints a one-shot diagnostic/angle check via Console.
 * Call after Console_Init(), with the sensor powered and SPI handle valid.
 */
void AS5047P_Init(SPI_HandleTypeDef *spi);
void AS5047P_Task(void);
bool AS5047P_ProcessCommand(const char *command);

#endif /* AS5047P_H */
