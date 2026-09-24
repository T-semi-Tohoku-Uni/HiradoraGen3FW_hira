#ifndef FOC_VOLTAGE_H
#define FOC_VOLTAGE_H
#include <stdbool.h>
#include <stdint.h>
bool FocVoltage_IsActive(void);
bool FocVoltage_ProcessCommand(const char *command);
void FocVoltage_Task(void);
void FocVoltage_ReportTask(void);
void FocVoltage_TickISR(void);
void FocVoltage_WatchdogISR(void);
void FocVoltage_AdcCompleteISR(void);
void FocVoltage_CurrentISR(const float currents[4], bool rails);
void FocVoltage_TripISR(const char *reason);
void FocVoltage_CheckDeadlineISR(uint32_t elapsed_cycles, bool late);
#endif
