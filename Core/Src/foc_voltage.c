#include "foc_voltage.h"
#include "motor_control.h"
#include "motor_calibration.h"
#include "motor_control_config.h"
#include "as5047p.h"
#include "bus_voltage.h"
#include "current_sense.h"
#include "voltage_vector.h"
#include "current_pi.h"
#include <math.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define PI 3.14159265358979323846f
#define TWO_PI (2.0f*PI)
static volatile bool active, running, adc_seen;
static const char *volatile fault;
static CalibrationRecord calibration;
static volatile float target_d, target_q, applied_d, applied_q;
typedef enum { VOLTAGE_CONTROL_MODE = 0, CURRENT_CONTROL_MODE } FocControlMode;
static volatile FocControlMode control_mode = VOLTAGE_CONTROL_MODE;
static volatile float current_target_d, current_target_q;
static float current_ref_d, current_ref_q, control_period;
static CurrentPi_State current_pi;
static const CurrentPi_Config current_pi_config = {
  MOTOR_CONTROL_CURRENT_KP_D, MOTOR_CONTROL_CURRENT_KI_D,
  MOTOR_CONTROL_CURRENT_KP_Q, MOTOR_CONTROL_CURRENT_KI_Q,
  MOTOR_CONTROL_CURRENT_INTEGRAL_LIMIT_D, MOTOR_CONTROL_CURRENT_INTEGRAL_LIMIT_Q
};
static void ResetCurrentControl(void)
{
  CurrentPi_Reset(&current_pi);
  current_ref_d = current_ref_q = 0.0f;
}
static volatile float vm_cache, electrical, rpm, measured_d, measured_q, peak;
static volatile uint32_t heartbeat, current_tick, started_ms, ticks, compute_max, total_max, age_max;
static volatile uint32_t adc_complete_cycles;
static volatile bool adc_completed;
static float observed_electrical;
static float observed_base_electrical, observed_correction;
static volatile bool h2_enabled; /* Boot OFF; no Flash persistence. */
static volatile int h2_gain = 1;
/* 同一ADC周期でPark変換と電圧生成が参照する、不変の角度スナップショット。 */
static AS5047P_Sample cycle_angle;
static uint32_t observation_adc_sequence, observation_adc_cycles;
static bool observation_valid;
bool FocVoltage_GetObservationISR(FocVoltage_Observation *observation)
{
  if (!observation_valid || !running || fault) return false;
  observation->id_a=measured_d;
  observation->iq_a=measured_q;
  observation->electrical_rad=observed_electrical;
  observation->mechanical_raw_rad=cycle_angle.mechanical_rad;
  observation->electrical_raw_rad=cycle_angle.electrical_rad;
  observation->electrical_base_rad=observed_base_electrical;
  observation->electrical_correction_rad=observed_correction;
  observation->adc_sequence=observation_adc_sequence;
  observation->angle_sequence=cycle_angle.sequence;
  observation->adc_callback_cycles=observation_adc_cycles;
  observation->angle_request_cycles=cycle_angle.request_cycles;
  observation->angle_received_cycles=cycle_angle.received_cycles;
  return true;
}
static uint32_t last_tick_cycles, angle_sequence, angle_cycles, reverse_since;
static float previous_angle;
/* 静止判定はmainで継続観測。停止指令を出しただけでは再始動を許可しない。 */
static bool still_tracking;
static uint32_t still_since;
static float still_anchor;
static float Difference(float a, float b)
{
  float d=a-b;
  if (d>PI) d-=TWO_PI;
  if (d< -PI) d+=TWO_PI;
  return d;
}
static bool Same(const char *s, const char *word)
{
  while (*word) if (tolower((unsigned char)*s++) != *word++) return false;
  while (isspace((unsigned char)*s)) s++;
  return !*s;
}
bool FocVoltage_IsActive(void) { return active; }
void FocVoltage_TripISR(const char *reason)
{
  if (!active) return;
  uint32_t mask=__get_PRIMASK(); __disable_irq();
  if (!fault) fault=reason;
  running=false;
  MotorControl_Stop();
  ResetCurrentControl();
  __set_PRIMASK(mask);
}
static bool ConfigValid(void)
{
  return isfinite(MOTOR_CONTROL_CURRENT_LIMIT_A) && MOTOR_CONTROL_CURRENT_LIMIT_A>0.0f &&
    isfinite(MOTOR_CONTROL_FOC_MAX_VOLTS) && MOTOR_CONTROL_FOC_MAX_VOLTS>0.0f &&
    MOTOR_CONTROL_FOC_MAX_VOLTS<=MOTOR_CONTROL_VOLTAGE_LIMIT &&
    isfinite(MOTOR_CONTROL_PWM_MARGIN) && MOTOR_CONTROL_PWM_MARGIN>=0.05f && MOTOR_CONTROL_PWM_MARGIN<0.5f &&
    isfinite(MOTOR_CONTROL_VM_MIN_VOLTS) && isfinite(MOTOR_CONTROL_VM_MAX_VOLTS) &&
    MOTOR_CONTROL_VM_MIN_VOLTS>0.0f && MOTOR_CONTROL_VM_MAX_VOLTS>MOTOR_CONTROL_VM_MIN_VOLTS &&
    isfinite(MOTOR_CONTROL_FOC_SLEW_VOLTS_PER_SEC) && MOTOR_CONTROL_FOC_SLEW_VOLTS_PER_SEC>0.0f &&
    isfinite(MOTOR_CONTROL_FOC_MAX_RPM) && MOTOR_CONTROL_FOC_MAX_RPM>0.0f &&
    MOTOR_CONTROL_FOC_ANGLE_MAX_AGE_US>0U && MOTOR_CONTROL_FOC_MAIN_TIMEOUT_MS>0U &&
    MOTOR_CONTROL_FOC_STANDSTILL_MS>0U && isfinite(MOTOR_CONTROL_FOC_STANDSTILL_RAD) &&
    MOTOR_CONTROL_FOC_STANDSTILL_RAD>0.0f &&
    fabsf(MOTOR_CONTROL_FOC_CURRENT_POLARITY)==1.0f;
}
static float Approach(float value,float goal,float step)
{
  if (goal>value) { float next=value+step; return next<goal ? next : goal; }
  float next=value-step; return next>goal ? next : goal;
}
static const char *CurrentConfigError(void)
{
  if (!CurrentPi_ConfigValid(&current_pi_config))
    return "PI gains must be finite/nonnegative; integral limits must be finite/positive";
  if (!isfinite(MOTOR_CONTROL_CURRENT_REF_MAX_A) || MOTOR_CONTROL_CURRENT_REF_MAX_A<=0.0f)
    return "CURRENT_REF_MAX_A must be finite and > 0";
  if (!(MOTOR_CONTROL_CURRENT_REF_MAX_A<MOTOR_CONTROL_CURRENT_LIMIT_A))
    return "CURRENT_REF_MAX_A must be < CURRENT_LIMIT_A (equality is not allowed)";
  if (!isfinite(MOTOR_CONTROL_CURRENT_SLEW_A_PER_SEC) || MOTOR_CONTROL_CURRENT_SLEW_A_PER_SEC<=0.0f)
    return "CURRENT_SLEW_A_PER_SEC must be finite and > 0";
  if (!isfinite(MOTOR_CONTROL_CURRENT_MAX_VOLTS) || MOTOR_CONTROL_CURRENT_MAX_VOLTS<=0.0f ||
      MOTOR_CONTROL_CURRENT_MAX_VOLTS>MOTOR_CONTROL_VOLTAGE_LIMIT)
    return "CURRENT_MAX_VOLTS must be finite, > 0 and <= VOLTAGE_LIMIT";
  return NULL;
}
static bool UpdateCurrentControl(void)
{
  /* Slew along the reference vector so intermediate references stay in the
   * permitted current circle, including during reversals. */
  float d=current_target_d-current_ref_d, q=current_target_q-current_ref_q;
  float distance=sqrtf(d*d+q*q);
  float step=MOTOR_CONTROL_CURRENT_SLEW_A_PER_SEC*control_period;
  float scale=distance>step ? step/distance : 1.0f;
  current_ref_d+=d*scale;
  current_ref_q+=q*scale;
  float limit=vm_cache*(1.0f-2.0f*MOTOR_CONTROL_PWM_MARGIN)/1.732050808f;
  if (limit>MOTOR_CONTROL_CURRENT_MAX_VOLTS) limit=MOTOR_CONTROL_CURRENT_MAX_VOLTS;
  if (limit>MOTOR_CONTROL_VOLTAGE_LIMIT) limit=MOTOR_CONTROL_VOLTAGE_LIMIT;
  if (!CurrentPi_Update(&current_pi,&current_pi_config,current_ref_d,current_ref_q,
                        measured_d,measured_q,control_period,limit)) return false;
  applied_d=current_pi.vd;
  applied_q=current_pi.vq;
  return true;
}
/* ADCが止まるとADC内の監視も止まるため、TIM更新IRQから別途呼ぶ。 */
void FocVoltage_WatchdogISR(void)
{
  if (!active || fault) return;
  uint32_t now=HAL_GetTick();
  /* bootstrap/I2C準備中にはまだADCを開始していない。最初の有効ADCまで
   * 開始用期限で監視し、運転中の2ms監視を誤適用しない。 */
  if (!running && !adc_seen) {
    if ((uint32_t)(now-started_ms)>MOTOR_CONTROL_FOC_MAIN_TIMEOUT_MS)
      FocVoltage_TripISR("arming watchdog");
    return;
  }
  if ((uint32_t)(now-heartbeat)>MOTOR_CONTROL_FOC_MAIN_TIMEOUT_MS)
    FocVoltage_TripISR("main stale");
  else if ((uint32_t)(now-current_tick)>2U)
    FocVoltage_TripISR("ADC stale");
  else if (running && adc_completed &&
      (uint32_t)(DWT->CYCCNT-adc_complete_cycles)>SystemCoreClock/10000U)
    FocVoltage_TripISR("ADC control period missed");
  else if (HAL_GPIO_ReadPin(GPIOE,GPIO_PIN_15)==GPIO_PIN_RESET)
    FocVoltage_TripISR("gate driver nFAULT");
}
void FocVoltage_AdcCompleteISR(void)
{
  adc_complete_cycles=DWT->CYCCNT;
  adc_completed=true;
}
void FocVoltage_TickISR(void)
{
  if (!running) {
    if (active && !fault && (uint32_t)(HAL_GetTick()-started_ms)>MOTOR_CONTROL_FOC_MAIN_TIMEOUT_MS)
      FocVoltage_TripISR("arming watchdog");
    return;
  }
  uint32_t began=DWT->CYCCNT, now=HAL_GetTick();
  if (!MotorControl_IsVoltageMode()) { FocVoltage_TripISR("PWM mode lost"); return; }
  if ((uint32_t)(now-heartbeat)>MOTOR_CONTROL_FOC_MAIN_TIMEOUT_MS) { FocVoltage_TripISR("main stale"); return; }
  if ((uint32_t)(now-current_tick)>2U) { FocVoltage_TripISR("ADC stale"); return; }
  if (HAL_GPIO_ReadPin(GPIOE,GPIO_PIN_15)==GPIO_PIN_RESET) { FocVoltage_TripISR("gate driver nFAULT"); return; }
  /* CurrentISRで取得・検証済み。ここで再取得するとdq電流と出力角がずれる。 */
  if (!observation_valid) { FocVoltage_TripISR("current observation invalid"); return; }
  const AS5047P_Sample *sample=&cycle_angle;
  uint32_t age=began-sample->request_cycles;
  if (age>age_max) age_max=age;
  if (age>(SystemCoreClock/1000000U)*MOTOR_CONTROL_FOC_ANGLE_MAX_AGE_US) {
    FocVoltage_TripISR("encoder age limit"); return;
  }
  if (sample->sequence!=angle_sequence) {
    uint32_t elapsed=sample->request_cycles-angle_cycles;
    if (elapsed) {
      float speed=Difference(sample->mechanical_rad,previous_angle)*
          (60.0f/TWO_PI)*(float)SystemCoreClock/(float)elapsed;
      /* 14bitの量子化を20kHzで差分すると速度に段差が出るため平滑化する。 */
      rpm+=(speed-rpm)*(1.0f/64.0f);
    }
    angle_cycles=sample->request_cycles; angle_sequence=sample->sequence;
    previous_angle=sample->mechanical_rad;
  }
  if (fabsf(rpm)>MOTOR_CONTROL_FOC_MAX_RPM) { FocVoltage_TripISR("speed limit"); return; }
  if (control_mode==VOLTAGE_CONTROL_MODE && rpm*(target_q>0.0f ? 1.0f : -1.0f)<-30.0f) {
    if (!reverse_since) reverse_since=now;
    if ((uint32_t)(now-reverse_since)>100U) { FocVoltage_TripISR("unexpected reverse rotation"); return; }
  } else reverse_since=0;
  uint32_t elapsed=last_tick_cycles ? began-last_tick_cycles : 0U;
  last_tick_cycles=began;
  if (elapsed>SystemCoreClock/5000U) { FocVoltage_TripISR("PWM period missed"); return; }
  if (control_mode==CURRENT_CONTROL_MODE) {
    if (!UpdateCurrentControl()) { FocVoltage_TripISR("current PI invalid"); return; }
  } else {
    float step=MOTOR_CONTROL_FOC_SLEW_VOLTS_PER_SEC*(float)elapsed/(float)SystemCoreClock;
    applied_d=Approach(applied_d,target_d,step);
    applied_q=Approach(applied_q,target_q,step);
  }
  electrical=observed_electrical;
  /* UVW座標で負の相順ならqも反転し、encoder増加方向のトルクを正にする。
   * 初版は角度外挿なし。前周期の取得角を使用し、古さを監視する。
   * CCRはこのISR後、次の頂点で反映（RCR=1）。センサー内部遅延は別途存在する。 */
  if (!MotorControl_SetVoltage(electrical,applied_d,
      applied_q*(float)calibration.direction,vm_cache)) {
    FocVoltage_TripISR("voltage output rejected"); return;
  }
  ticks++;
  uint32_t spent=DWT->CYCCNT-began;
  if (spent>compute_max) compute_max=spent;
}
void FocVoltage_CheckDeadlineISR(uint32_t elapsed_cycles,bool late)
{
  if (!running) return;
  if (elapsed_cycles>total_max) total_max=elapsed_cycles;
  if (late) FocVoltage_TripISR("PWM update deadline");
}
void FocVoltage_CurrentISR(const float currents[4],bool rails,
                           uint32_t adc_sequence,uint32_t adc_callback_cycles)
{
  observation_valid=false;
  if (!active || fault) return;
  current_tick=HAL_GetTick();
  if (rails) { FocVoltage_TripISR("ADC saturated"); return; }
  for (unsigned i=0;i<4;i++) {
    if (fabsf(currents[i])>peak) peak=fabsf(currents[i]);
    if (!isfinite(currents[i]) || fabsf(currents[i])>MOTOR_CONTROL_CURRENT_LIMIT_A) {
      FocVoltage_TripISR("phase overcurrent"); return;
    }
  }
  adc_seen=true;
  if (!running) return; /* 起動準備中は電流保護だけ行い、観測値を公開しない。 */
  if (!AS5047P_GetSample(&cycle_angle)) { FocVoltage_TripISR("encoder invalid"); return; }
  uint32_t age=DWT->CYCCNT-cycle_angle.request_cycles;
  if (age>(SystemCoreClock/1000000U)*MOTOR_CONTROL_FOC_ANGLE_MAX_AGE_US) {
    FocVoltage_TripISR("encoder age limit"); return;
  }
  observation_adc_sequence=adc_sequence;
  observation_adc_cycles=adc_callback_cycles;
  /* 3相の共通成分を除いてClarke変換する。電流PIも同じdq観測を使用。
   * Uは2ランクの平均、V/Wも採用。サンプル時刻は完全同時ではない。 */
  float u=(currents[0]+currents[2])*0.5f*MOTOR_CONTROL_FOC_CURRENT_POLARITY;
  float v=currents[1]*MOTOR_CONTROL_FOC_CURRENT_POLARITY;
  float w=currents[3]*MOTOR_CONTROL_FOC_CURRENT_POLARITY;
  float a=(2.0f*u-v-w)/3.0f, b=(v-w)/1.732050808f;
  float c,s;
  /* 今回のADCに組み合わせる最新角度。前周期のelectricalは再利用しない。
   * この角度をTickISRにも渡す。センサー遅延を仮定した外挿はまだ行わない。 */
  observed_base_electrical=VoltageVector_Wrap((float)calibration.direction*MOTOR_CONTROL_POLE_PAIRS*
                                       cycle_angle.mechanical_rad-calibration.offset);
  observed_correction=0.0f;
  if (h2_enabled && h2_gain!=0) {
    float error_sin, error_cos;
    VoltageVector_SinCos(2.0f*cycle_angle.mechanical_rad,&error_sin,&error_cos);
    /* Error is specified directly in electrical radians in the FOC frame. */
    observed_correction=-(float)h2_gain*
        (calibration.h2_cos_rad_elec*error_cos+calibration.h2_sin_rad_elec*error_sin);
  }
  /* Preserve the original path exactly when disabled or gain is zero.
   * Park and inverse Park share this one corrected snapshot. */
  observed_electrical=observed_correction==0.0f ? observed_base_electrical :
      VoltageVector_Wrap(observed_base_electrical+observed_correction);
  VoltageVector_SinCos(observed_electrical,&s,&c);
  measured_d=a*c+b*s;
  measured_q=(-a*s+b*c)*(float)calibration.direction;
  observation_valid=running;
}
void FocVoltage_Task(void)
{
  AS5047P_Sample sample;
  bool angle_ok=AS5047P_GetSample(&sample);
  uint32_t now=HAL_GetTick();
  if (!active) {
    if (!MotorControl_IsStopped() || !angle_ok) { still_tracking=false; return; }
    if (!still_tracking || fabsf(Difference(sample.mechanical_rad,still_anchor))>MOTOR_CONTROL_FOC_STANDSTILL_RAD) {
      still_tracking=true; still_since=now; still_anchor=sample.mechanical_rad;
    }
    return;
  }
  if (fault || !MotorControl_IsVoltageMode()) {
    MotorControl_Stop(); running=false; CurrentSense_EndControl();
    active=false; still_tracking=false;
    target_d=target_q=applied_d=applied_q=0.0f;
    current_target_d=current_target_q=0.0f;
    ResetCurrentControl();
    printf("FOC stopped: %s, peak=%.3f A\r\n",fault ? fault : "PWM stopped",(double)peak);
    return;
  }
  BusVoltageSample vm;
  if (!BusVoltage_GetSample(&vm) || !isfinite(vm.volts) || vm.overvoltage ||
      vm.volts<MOTOR_CONTROL_VM_MIN_VOLTS ||
      (uint32_t)(now-vm.updated_ms)>MOTOR_CONTROL_FOC_MAIN_TIMEOUT_MS || !angle_ok) {
    FocVoltage_TripISR("VM/encoder invalid or stale"); return;
  }
  vm_cache=vm.volts; heartbeat=now;
  if (!running) {
    /* PWMゼロ電圧のままSPIとADCの最初の周期を待つ。古いmain取得角で開始しない。 */
    if (adc_seen && sample.sequence!=angle_sequence &&
        (uint32_t)(DWT->CYCCNT-sample.request_cycles)<(SystemCoreClock/1000000U)*MOTOR_CONTROL_FOC_ANGLE_MAX_AGE_US &&
        (uint32_t)(now-current_tick)<=2U) {
      uint32_t mask=__get_PRIMASK(); __disable_irq();
      if (!fault) {
        angle_cycles=sample.request_cycles; angle_sequence=sample.sequence;
        previous_angle=sample.mechanical_rad; last_tick_cycles=0;
        running=true;
      }
      __set_PRIMASK(mask);
    } else if ((uint32_t)(now-started_ms)>10U) FocVoltage_TripISR("acquisition start timeout");
  }
}
static void PrintStatus(bool capture)
{
  static unsigned line;
  static float td,tq,d,q,e,speed,id,iq,p;
  static uint32_t n,c,t,a;
  static bool on,run;
  static FocControlMode mode;
  static float itd,itq,ird,irq,intd,intq;
  static bool saturated;
  static const char *why;
  if(capture) {
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    td=target_d; tq=target_q; d=applied_d; q=applied_q; e=electrical;
    speed=rpm; id=measured_d; iq=measured_q; p=peak;
    n=ticks; c=compute_max; t=total_max; a=age_max;
    on=active; run=running; why=fault;
    mode=control_mode; itd=current_target_d; itq=current_target_q;
    ird=current_ref_d; irq=current_ref_q;
    intd=current_pi.integral_d; intq=current_pi.integral_q;
    saturated=current_pi.saturated;
    __set_PRIMASK(mask);
    line=1; return;
  }
  if(!line) return;
  /* 1回のmainループで1行だけ送る。行間にVM取得とheartbeat更新を挟む。 */
  /* 周期割り込みの合間に多数の%fを整形するとmain監視期限を超える。
   * 状態表示はmV/mA/mradと整数nsへ変換し、printfの浮動小数点整形を避ける。 */
  if(line==1) printf("FOC: %s, mode=%s, target=%ld/%ld mV, applied=%ld/%ld mV, elec=%ld mrad, rpm=%ld\r\n",
      on ? (run ? "running" : "arming/stopping") : "off",
      mode==CURRENT_CONTROL_MODE ? "current" : "voltage",
      (long)(td*1000),(long)(tq*1000),(long)(d*1000),(long)(q*1000),(long)(e*1000),(long)speed);
  else if(line==2) printf("FOC current: id=%ld iq=%ld mA (configured polarity), peak=%ld mA; fault=%s\r\n",
      (long)(id*1000),(long)(iq*1000),(long)(p*1000),why ? why : "none");
  else if(line==3) printf("FOC timing: ticks=%lu, compute_max=%lu ns, adc_control_max=%lu ns, angle_age_max=%lu ns\r\n",
      (unsigned long)n,(unsigned long)((float)c*(1e9f/(float)SystemCoreClock)),
      (unsigned long)((float)t*(1e9f/(float)SystemCoreClock)),
      (unsigned long)((float)a*(1e9f/(float)SystemCoreClock)));
  else if(line==4) MotorControl_PrintFocPhase();
  else if(line==5) printf("FOC PI: target=%ld/%ld mA, ref=%ld/%ld mA\r\n",
      (long)(itd*1000),(long)(itq*1000),(long)(ird*1000),(long)(irq*1000));
  else printf("FOC PI: integral=%ld/%ld mV, saturated=%u\r\n",
      (long)(intd*1000),(long)(intq*1000),(unsigned)saturated);
  line=line==(mode==CURRENT_CONTROL_MODE ? 6U : 4U) ? 0 : line+1;
}
void FocVoltage_ReportTask(void) { PrintStatus(false); }
bool FocVoltage_ProcessCommand(const char *command)
{
  if (!command) return false;
  while (isspace((unsigned char)*command)) command++;
  if (strlen(command)<3 || tolower((unsigned char)command[0])!='f' ||
      tolower((unsigned char)command[1])!='o' || tolower((unsigned char)command[2])!='c' ||
      (command[3] && !isspace((unsigned char)command[3]))) return false;
  const char *arg=command+3; while (isspace((unsigned char)*arg)) arg++;
  if (!*arg || Same(arg,"status")) PrintStatus(true);
  else if (strlen(arg)>=2 && !strncmp(arg,"h2",2) &&
           (!arg[2] || isspace((unsigned char)arg[2]))) {
    const char *setting=arg+2;
    while (isspace((unsigned char)*setting)) setting++;
    if (!*setting || Same(setting,"status")) { /* Read-only query. */ }
    else if (Same(setting,"on")) h2_enabled=true;
    else if (Same(setting,"off")) h2_enabled=false;
    else if (Same(setting,"gain 0")) h2_gain=0;
    else if (Same(setting,"gain +1") || Same(setting,"gain 1")) h2_gain=1;
    else if (Same(setting,"gain -1")) h2_gain=-1;
    else {
      printf("Usage: foc h2 on|off|gain <0|+1|-1>|status\r\n"); return true;
    }
    CalibrationRecord h2_record;
    bool valid;
    if (active) { h2_record=calibration; valid=true; }
    else valid=MotorCalibration_Get(&h2_record);
    printf("FOC h2: %s, gain=%d, valid=%u, a2=%ld b2=%ld urad_elec\r\n",
        h2_enabled ? "on" : "off",h2_gain,(unsigned)valid,
        valid ? (long)(h2_record.h2_cos_rad_elec*1e6f) : 0L,
        valid ? (long)(h2_record.h2_sin_rad_elec*1e6f) : 0L);
  }
  else if (Same(arg,"stop")) FocVoltage_TripISR("user stop");
  else if (Same(arg,"start")) {
    if (active || MotorCalibration_IsActive() || !MotorControl_IsStopped() || CurrentSense_IsBusy()) {
      printf("FOC start blocked: stop PWM/calibration/ADC log first\r\n"); return true;
    }
    AS5047P_Sample sample; BusVoltageSample vm;
    const char *config_error=CurrentConfigError();
    if (control_mode==CURRENT_CONTROL_MODE && config_error) {
      printf("FOC start blocked: config: %s\r\n",config_error); return true;
    }
    control_period=MotorControl_GetPeriodSeconds();
    if (!ConfigValid() || (control_mode==CURRENT_CONTROL_MODE &&
        (!isfinite(control_period) || control_period<=0.0f)) ||
        !MotorCalibration_Get(&calibration) ||
        !AS5047P_GetSample(&sample) || !BusVoltage_GetSample(&vm) || vm.overvoltage ||
        vm.volts<MOTOR_CONTROL_VM_MIN_VOLTS || !isfinite(vm.volts)) {
      printf("FOC start blocked: config/calibration/encoder/VM invalid\r\n"); return true;
    }
    if (!still_tracking || (uint32_t)(HAL_GetTick()-still_since)<MOTOR_CONTROL_FOC_STANDSTILL_MS) {
      printf("FOC start blocked: wait for stationary rotor\r\n"); return true;
    }
    if (control_mode==VOLTAGE_CONTROL_MODE && target_q==0.0f) {
      printf("Set nonzero Vq with foc voltage <Vd> <Vq> first\r\n"); return true;
    }
    ResetCurrentControl();
    observation_valid=false;
    fault=NULL; active=true; running=false; adc_seen=false; adc_completed=false;
    applied_d=applied_q=peak=rpm=measured_d=measured_q=0.0f;
    ticks=compute_max=total_max=age_max=reverse_since=0;
    angle_sequence=sample.sequence; vm_cache=vm.volts;
    current_tick=heartbeat=started_ms=HAL_GetTick();
    if (MotorControl_StartVoltage()!=HAL_OK || CurrentSense_BeginControl()!=HAL_OK) {
      FocVoltage_TripISR("PWM/ADC start failed"); return true;
    }
    current_tick=heartbeat=started_ms=HAL_GetTick();
    if (control_mode==CURRENT_CONTROL_MODE)
      printf("FOC current start: reference ramp from 0 A, phase trip=%ld mA\r\n",
          (long)(MOTOR_CONTROL_CURRENT_LIMIT_A*1000));
    else printf("FOC voltage start: ramp from 0 V, current limit %.2f A\r\n",(double)MOTOR_CONTROL_CURRENT_LIMIT_A);
  } else if (strlen(arg)>=7 && !strncmp(arg,"current",7) && isspace((unsigned char)arg[7])) {
    char *end; const char *a=arg+7; errno=0; float d=strtof(a,&end);
    if (end==a || !isspace((unsigned char)*end) || errno==ERANGE) {
      printf("Usage: foc current <Id_A> <Iq_A>\r\n"); return true;
    }
    a=end; errno=0; float q=strtof(a,&end);
    bool parsed_q=end!=a;
    while (isspace((unsigned char)*end)) end++;
    if (!parsed_q || *end || errno==ERANGE || !isfinite(d) || !isfinite(q) ||
        hypotf(d,q)>MOTOR_CONTROL_CURRENT_REF_MAX_A) {
      printf("FOC current rejected: finite vector <= %ld mA required\r\n",
          (long)(MOTOR_CONTROL_CURRENT_REF_MAX_A*1000)); return true;
    }
    const char *config_error=CurrentConfigError();
    if (config_error) {
      printf("FOC current rejected: config: %s\r\n",config_error); return true;
    }
    if (!ConfigValid()) {
      printf("FOC current rejected: common FOC config invalid (motor_control_config.h)\r\n"); return true;
    }
    if (MotorCalibration_IsActive()) { printf("Stop calibration first\r\n"); return true; }
    if (active && (control_mode!=CURRENT_CONTROL_MODE || fault)) {
      printf("FOC current rejected: stop before mode change/restart\r\n"); return true;
    }
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    if (!active) ResetCurrentControl();
    control_mode=CURRENT_CONTROL_MODE;
    current_target_d=d; current_target_q=q;
    target_d=target_q=0.0f;
    __set_PRIMASK(mask);
    printf("FOC current set: Id=%ld mA, Iq=%ld mA\r\n",(long)(d*1000),(long)(q*1000));
  } else if (strlen(arg)>=7 && !strncmp(arg,"voltage",7) && isspace((unsigned char)arg[7])) {
    char *end; const char *a=arg+7; errno=0; float d=strtof(a,&end);
    if (end==a || !isspace((unsigned char)*end) || errno==ERANGE) { printf("Usage: foc voltage <Vd> <Vq>\r\n"); return true; }
    a=end; errno=0; float q=strtof(a,&end);
    bool parsed_q=end!=a;
    while (isspace((unsigned char)*end)) end++;
    if (!parsed_q || *end || errno==ERANGE || !isfinite(d) || !isfinite(q) ||
        hypotf(d,q)>MOTOR_CONTROL_FOC_MAX_VOLTS || !ConfigValid()) {
      printf("FOC voltage rejected: finite vector magnitude <= %.3f V required\r\n",(double)MOTOR_CONTROL_FOC_MAX_VOLTS); return true;
    }
    if (MotorCalibration_IsActive()) { printf("Stop calibration first\r\n"); return true; }
    if (active && control_mode!=VOLTAGE_CONTROL_MODE) {
      printf("FOC voltage rejected: stop before mode change\r\n"); return true;
    }
    /* 下げる電圧や反転による能動的な制動を扱わない。0指令は全出力OFF。
     * 物理的な逆流を遮断する回路ではないので、VM監視は別途継続する。 */
    if (active && d==0.0f && q==0.0f) { FocVoltage_TripISR("zero voltage: coast stop"); return true; }
    if (active && (q*target_q<=0.0f || fabsf(q)<fabsf(target_q) || d!=target_d)) {
      printf("FOC voltage rejected: stop before reversal/decrease/Vd change\r\n"); return true;
    }
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    if (!active) ResetCurrentControl();
    control_mode=VOLTAGE_CONTROL_MODE;
    current_target_d=current_target_q=0.0f;
    target_d=d; target_q=q;
    __set_PRIMASK(mask);
    printf("FOC voltage set: Vd=%.3f V, Vq=%.3f V\r\n",(double)d,(double)q);
  } else printf("Usage: foc voltage <Vd> <Vq> | foc current <Id> <Iq> | foc start | foc status | foc stop | foc h2 on/off/gain/status\r\n");
  return true;
}
