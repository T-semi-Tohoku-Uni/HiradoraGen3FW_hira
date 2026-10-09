#ifndef FOC_VOLTAGE_H
#define FOC_VOLTAGE_H
#include <stdbool.h>
#include <stdint.h>
/* Shared FOC lifecycle: voltage command or current PI, selected while stopped.
 * foc current <Id_A> <Iq_A> / foc voltage <Vd_V> <Vq_V>, then foc start.
 * Zero current keeps regulation active; foc stop disables PWM. */
/* 同じADC ISR内でのみ取得する観測値。角度はdq変換に実際に使用したrad値。 */
typedef struct {
  float id_a, iq_a, electrical_rad;
  /* Raw sensor mechanical/electrical angles; base includes direction/offset.
   * correction is the signed electrical addition before wrapping (rad). */
  float mechanical_raw_rad, electrical_raw_rad, electrical_base_rad;
  float electrical_correction_rad;
  /* 電流PIと同じ組を観測できるよう、ADC番号と角度の出所を保存する。
   * cyclesはDWT（uint32_t周回）。ADC時刻はコールバック入口で、S/H時刻ではない。 */
  uint32_t adc_sequence, angle_sequence;
  uint32_t adc_callback_cycles, angle_request_cycles, angle_received_cycles;
} FocVoltage_Observation;
bool FocVoltage_GetObservationISR(FocVoltage_Observation *observation);
bool FocVoltage_IsActive(void);
bool FocVoltage_ProcessCommand(const char *command);
void FocVoltage_Task(void);
void FocVoltage_ReportTask(void);
void FocVoltage_TickISR(void);
void FocVoltage_WatchdogISR(void);
void FocVoltage_AdcCompleteISR(void);
void FocVoltage_CurrentISR(const float currents[4], bool rails,
                           uint32_t adc_sequence, uint32_t adc_callback_cycles);
void FocVoltage_TripISR(const char *reason);
void FocVoltage_CheckDeadlineISR(uint32_t elapsed_cycles, bool late);
#endif
