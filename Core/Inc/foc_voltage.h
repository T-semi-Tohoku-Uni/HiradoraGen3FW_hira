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
  uint32_t angle_reference_cycles, adc_sample_cycles, pwm_midpoint_cycles;
  float pwm_electrical_rad, electrical_velocity_rad_s;
} FocVoltage_Observation;
bool FocVoltage_GetObservationISR(FocVoltage_Observation *observation);
/* main専用。停止・異常・古い観測はfalse。電流PIを変更しない読み取りAPI。 */
bool FocVoltage_GetIqSample(float *iq_a);
bool FocVoltage_IsActive(void);
bool FocVoltage_IsRunning(void); /* Distinguish arming from ADC control. */
/* main専用。電流目標を同時更新する。PWM開始・fault解除は行わない。
 * 非有限値、既存の電流ベクトル上限超過、校正中、運転中のモード変更、
 * 停止処理前のfaultはfalseで拒否し、目標を保持する。
 * スルーレートは既存ADC制御周期で適用。呼出元でUART/CAN所有権を管理する。 */
bool Foc_SetCurrentTarget(float id_a, float iq_a);
/* main専用。ゼロ電流でPWM開始。許可関数はTIM ISR/排他区間でも呼ぶ。 */
bool Foc_StartCurrent(bool (*permitted)(void));
bool Foc_IsHealthy(void);
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
