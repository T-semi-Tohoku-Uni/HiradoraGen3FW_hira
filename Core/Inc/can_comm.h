#ifndef CAN_COMM_H
#define CAN_COMM_H
#include "stm32g4xx_hal.h"
#include <stdbool.h>
/* main専用。観測・安全状態の試験段階。CANからPWMを開始する経路はない。 */
void CanComm_Init(FDCAN_HandleTypeDef *handle);
void CanComm_Task(void);
bool CanComm_ProcessCommand(const char *command);
/* SysTickで状態だけを監視。CAN ISRからFOC/PWMを操作しない。 */
void CanComm_WatchdogISR(void);
#endif
