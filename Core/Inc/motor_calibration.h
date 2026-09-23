#ifndef MOTOR_CALIBRATION_H
#define MOTOR_CALIBRATION_H
#include <stdbool.h>
#include "calibration_store.h"
void MotorCalibration_Init(void);
void MotorCalibration_Task(void);
bool MotorCalibration_ProcessCommand(const char *command);
bool MotorCalibration_IsActive(void);
/* ISRでは停止と理由の記録だけ。printf/HAL Flash/I2Cは呼ばない。 */
void MotorCalibration_TripISR(const char *reason);
void MotorCalibration_CurrentISR(const float currents[4], bool rails);
void MotorCalibration_WatchdogISR(void);
/* 将来のFOC開始条件。未校正ならfalse。角度はUVW基準でdirection*p*mech-offset。
 * encoder増加方向のトルクを正にするため、逆相順ではVqもdirection倍する。 */
bool MotorCalibration_Get(CalibrationRecord *record);
#endif
