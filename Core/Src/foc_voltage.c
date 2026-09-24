#include "foc_voltage.h"
#include "irq_trace.h"
#include "motor_control.h"
#include "motor_calibration.h"
#include "motor_control_config.h"
#include "as5047p.h"
#include "bus_voltage.h"
#include "current_sense.h"
#include "voltage_vector.h"
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
static volatile float vm_cache, electrical, rpm, measured_d, measured_q, peak;
static volatile uint32_t heartbeat, current_tick, started_ms, ticks, compute_max, total_max, age_max;
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
  AS5047P_Sample sample;
  if (!AS5047P_GetSample(&sample)) { FocVoltage_TripISR("encoder invalid"); return; }
  uint32_t age=began-sample.request_cycles;
  if (age>age_max) age_max=age;
  if (age>(SystemCoreClock/1000000U)*MOTOR_CONTROL_FOC_ANGLE_MAX_AGE_US) {
    FocVoltage_TripISR("encoder age limit"); return;
  }
  if (sample.sequence!=angle_sequence) {
    uint32_t elapsed=sample.request_cycles-angle_cycles;
    if (elapsed) {
      float speed=Difference(sample.mechanical_rad,previous_angle)*
          (60.0f/TWO_PI)*(float)SystemCoreClock/(float)elapsed;
      /* 14bitの量子化を20kHzで差分すると速度に段差が出るため平滑化する。 */
      rpm+=(speed-rpm)*(1.0f/64.0f);
    }
    angle_cycles=sample.request_cycles; angle_sequence=sample.sequence;
    previous_angle=sample.mechanical_rad;
  }
  if (fabsf(rpm)>MOTOR_CONTROL_FOC_MAX_RPM) { FocVoltage_TripISR("speed limit"); return; }
  if (rpm*(target_q>0.0f ? 1.0f : -1.0f)<-30.0f) {
    if (!reverse_since) reverse_since=now;
    if ((uint32_t)(now-reverse_since)>100U) { FocVoltage_TripISR("unexpected reverse rotation"); return; }
  } else reverse_since=0;
  uint32_t elapsed=last_tick_cycles ? began-last_tick_cycles : 0U;
  last_tick_cycles=began;
  if (elapsed>SystemCoreClock/5000U) { FocVoltage_TripISR("PWM period missed"); return; }
  float step=MOTOR_CONTROL_FOC_SLEW_VOLTS_PER_SEC*(float)elapsed/(float)SystemCoreClock;
  applied_d=Approach(applied_d,target_d,step);
  applied_q=Approach(applied_q,target_q,step);
  electrical=VoltageVector_Wrap((float)calibration.direction*MOTOR_CONTROL_POLE_PAIRS*
                              sample.mechanical_rad-calibration.offset);
  /* UVW座標で負の相順ならqも反転し、encoder増加方向のトルクを正にする。
   * 初版は角度外挿なし。前周期の取得角を使用し、古さを監視する。
   * CCRはこのISR後、次の底で反映（RCR=1）。センサー内部遅延は別途存在する。 */
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
void FocVoltage_CurrentISR(const float currents[4],bool rails)
{
  if (!active) return;
  current_tick=HAL_GetTick();
  if (rails) { FocVoltage_TripISR("ADC saturated"); return; }
  for (unsigned i=0;i<4;i++) {
    if (fabsf(currents[i])>peak) peak=fabsf(currents[i]);
    if (!isfinite(currents[i]) || fabsf(currents[i])>MOTOR_CONTROL_CURRENT_LIMIT_A) {
      FocVoltage_TripISR("phase overcurrent"); return;
    }
  }
  adc_seen=true;
  /* dqは観測のみ。電流PIは未実装。3相の共通成分を除いてClarke変換する。
   * Uは2ランクの平均、V/Wも採用。サンプル時刻は完全同時ではない。 */
  float u=(currents[0]+currents[2])*0.5f*MOTOR_CONTROL_FOC_CURRENT_POLARITY;
  float v=currents[1]*MOTOR_CONTROL_FOC_CURRENT_POLARITY;
  float w=currents[3]*MOTOR_CONTROL_FOC_CURRENT_POLARITY;
  float a=(2.0f*u-v-w)/3.0f, b=(v-w)/1.732050808f;
  float c,s;
  VoltageVector_SinCos(electrical,&s,&c);
  measured_d=a*c+b*s;
  measured_q=(-a*s+b*c)*(float)calibration.direction;
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
        IrqTrace_Begin();
        running=true;
      }
      __set_PRIMASK(mask);
    } else if ((uint32_t)(now-started_ms)>10U) FocVoltage_TripISR("acquisition start timeout");
  }
}
static void PrintStatus(void)
{
  /* 文字列整形中は割り込みを止めず、数値のコピーだけを排他する。 */
  uint32_t mask=__get_PRIMASK(); __disable_irq();
  float td=target_d,tq=target_q,d=applied_d,q=applied_q,e=electrical,speed=rpm,id=measured_d,iq=measured_q,p=peak;
  uint32_t n=ticks,c=compute_max,t=total_max,a=age_max;
  bool on=active,run=running; const char *why=fault;
  __set_PRIMASK(mask);
  /* 周期割り込みの合間に多数の%fを整形するとmain監視期限を超える。
   * 状態表示はmV/mA/mradと整数nsへ変換し、printfの浮動小数点整形を避ける。 */
  printf("FOC: %s, target=%ld/%ld mV, applied=%ld/%ld mV, elec=%ld mrad, rpm=%ld\r\n",
      on ? (run ? "running" : "arming/stopping") : "off",
      (long)(td*1000),(long)(tq*1000),(long)(d*1000),(long)(q*1000),(long)(e*1000),(long)speed);
  printf("FOC current: id=%ld iq=%ld mA (ADC polarity), peak=%ld mA; fault=%s\r\n",
      (long)(id*1000),(long)(iq*1000),(long)(p*1000),why ? why : "none");
  printf("FOC timing: ticks=%lu, compute_max=%lu ns, bottom_max=%lu ns, angle_age_max=%lu ns\r\n",
      (unsigned long)n,(unsigned long)((float)c*(1e9f/(float)SystemCoreClock)),
      (unsigned long)((float)t*(1e9f/(float)SystemCoreClock)),
      (unsigned long)((float)a*(1e9f/(float)SystemCoreClock)));
}
bool FocVoltage_ProcessCommand(const char *command)
{
  if (!command) return false;
  while (isspace((unsigned char)*command)) command++;
  if (strlen(command)<3 || tolower((unsigned char)command[0])!='f' ||
      tolower((unsigned char)command[1])!='o' || tolower((unsigned char)command[2])!='c' ||
      (command[3] && !isspace((unsigned char)command[3]))) return false;
  const char *arg=command+3; while (isspace((unsigned char)*arg)) arg++;
  if (!*arg || Same(arg,"status")) PrintStatus();
  else if (Same(arg,"stop")) FocVoltage_TripISR("user stop");
  else if (Same(arg,"start")) {
    if (active || MotorCalibration_IsActive() || !MotorControl_IsStopped() || CurrentSense_IsBusy()) {
      printf("FOC start blocked: stop PWM/calibration/ADC log first\r\n"); return true;
    }
    AS5047P_Sample sample; BusVoltageSample vm;
    if (!ConfigValid() || !MotorCalibration_Get(&calibration) ||
        !AS5047P_GetSample(&sample) || !BusVoltage_GetSample(&vm) || vm.overvoltage ||
        vm.volts<MOTOR_CONTROL_VM_MIN_VOLTS || !isfinite(vm.volts)) {
      printf("FOC start blocked: config/calibration/encoder/VM invalid\r\n"); return true;
    }
    if (!still_tracking || (uint32_t)(HAL_GetTick()-still_since)<MOTOR_CONTROL_FOC_STANDSTILL_MS) {
      printf("FOC start blocked: wait for stationary rotor\r\n"); return true;
    }
    if (target_q==0.0f) { printf("Set nonzero Vq with foc voltage <Vd> <Vq> first\r\n"); return true; }
    fault=NULL; active=true; running=false; adc_seen=false;
    applied_d=applied_q=peak=rpm=measured_d=measured_q=0.0f;
    ticks=compute_max=total_max=age_max=reverse_since=0;
    angle_sequence=sample.sequence; vm_cache=vm.volts;
    current_tick=heartbeat=started_ms=HAL_GetTick();
    if (MotorControl_StartVoltage()!=HAL_OK || CurrentSense_BeginControl()!=HAL_OK) {
      FocVoltage_TripISR("PWM/ADC start failed"); return true;
    }
    current_tick=heartbeat=started_ms=HAL_GetTick();
    printf("FOC voltage start: ramp from 0 V, current limit %.2f A\r\n",(double)MOTOR_CONTROL_CURRENT_LIMIT_A);
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
    /* 下げる電圧や反転による能動的な制動を扱わない。0指令は全出力OFF。
     * 物理的な逆流を遮断する回路ではないので、VM監視は別途継続する。 */
    if (active && d==0.0f && q==0.0f) { FocVoltage_TripISR("zero voltage: coast stop"); return true; }
    if (active && (q*target_q<=0.0f || fabsf(q)<fabsf(target_q) || d!=target_d)) {
      printf("FOC voltage rejected: stop before reversal/decrease/Vd change\r\n"); return true;
    }
    uint32_t mask=__get_PRIMASK(); __disable_irq(); target_d=d; target_q=q; __set_PRIMASK(mask);
    printf("FOC voltage set: Vd=%.3f V, Vq=%.3f V\r\n",(double)d,(double)q);
  } else printf("Usage: foc voltage <Vd> <Vq> | foc start | foc status | foc stop\r\n");
  return true;
}
