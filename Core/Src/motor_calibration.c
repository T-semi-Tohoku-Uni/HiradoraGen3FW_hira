#include "motor_calibration.h"
#include "motor_control.h"
#include "motor_control_config.h"
#include "current_sense.h"
#include "bus_voltage.h"
#include "as5047p.h"
#include "voltage_vector.h"
#include "console.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#define PI 3.14159265358979323846f
#define TWO_PI (2.0f*PI)
typedef enum { IDLE, CHECK_STILL, RAMP, HOLD_ZERO, FORWARD, HOLD_END, BACKWARD, HOLD_RETURN, APPROACH, HOLD_APPROACH } CalStage;
static volatile CalStage stage;
static CalibrationRecord record;
static bool calibrated, saved;
/* ログの間引きで消える瞬間的な過電流も、停止したサンプルをISRで保存する。
 * 出力はPWM停止後のmainのみ。保護判定を遅らせない。 */
static volatile float trip_current[4], peak_current, requested_voltage;
static volatile float observed_current[4];
static volatile uint32_t trip_stage, trip_elapsed;
static volatile bool trip_captured;
static const char *volatile fault;
static volatile uint32_t heartbeat, current_ms;
static volatile bool energized;
static uint32_t stage_ms, task_ms, sequence;
static float previous_angle, position, origin, end_position, approach_start;
static float sum, minimum, maximum;
static unsigned count;
static bool Same(const char *text, const char *expected)
{
  while (*expected) if (tolower((unsigned char)*text++) != *expected++) return false;
  while (isspace((unsigned char)*text)) text++;
  return *text == 0;
}
bool MotorCalibration_IsActive(void) { return stage != IDLE; }
bool MotorCalibration_Get(CalibrationRecord *r)
{
  if (!calibrated || MotorCalibration_IsActive() || !r) return false;
  *r = record; return true;
}
void MotorCalibration_TripISR(const char *reason)
{
  if (!MotorCalibration_IsActive()) return;
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  if (!fault) fault = reason;
  MotorControl_Stop();
  __set_PRIMASK(mask);
}
void MotorCalibration_CurrentISR(const float currents[4], bool rails)
{
  if (!MotorCalibration_IsActive()) return;
  current_ms = HAL_GetTick();
  for (unsigned i=0; i<4; i++) observed_current[i] = currents[i];
  if (rails) { MotorCalibration_TripISR("ADC saturated"); return; }
  for (unsigned i=0; i<4; i++) {
    if (fabsf(currents[i]) > peak_current) peak_current = fabsf(currents[i]);
    /* 校正は専用の10A閾値で判定。通常運転用の5Aを重ねて適用しない。
     * NaN/InfとADC飽和の停止は従来どおり維持する。 */
    if (!isfinite(currents[i]) || fabsf(currents[i]) > MOTOR_CONTROL_CAL_CURRENT_LIMIT_A) {
      if (!trip_captured) {
        for (unsigned j=0; j<4; j++) trip_current[j] = currents[j];
        trip_stage = (uint32_t)stage; trip_elapsed = HAL_GetTick()-stage_ms;
        trip_captured = true;
      }
      MotorCalibration_TripISR("phase overcurrent"); return;
    }
  }
}
void MotorCalibration_WatchdogISR(void)
{
  if (!energized) return;
  uint32_t now = HAL_GetTick();
  if ((uint32_t)(now-heartbeat) > MOTOR_CONTROL_CAL_WATCHDOG_MS)
    MotorCalibration_TripISR("main/voltage/encoder watchdog");
  if ((uint32_t)(now-current_ms) > 5U) MotorCalibration_TripISR("ADC stale");
  if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_15) == GPIO_PIN_RESET)
    MotorCalibration_TripISR("gate driver nFAULT");
}
static void Enter(CalStage next, uint32_t now)
{
  stage = next; stage_ms = now;
  sum = 0.0f; count = 0; minimum = INFINITY; maximum = -INFINITY;
}
static void Finish(const char *reason)
{
  /* 出力停止を先に行い、その後でADCとログを閉じる。Flash操作は別の明示コマンド。 */
  MotorControl_Stop(); energized = false;
  CurrentSense_EndControl(); stage = IDLE;
  printf("Calibration %s; PWM stopped\r\n", reason);
  printf("Calibration peak (all ADC samples): %.3f A\r\n", (double)peak_current);
  if (trip_captured) printf("Calibration trip: stage=%lu, elapsed=%lu ms, requested=%.3f V, "
      "U1=%.3f V=%.3f U2=%.3f W=%.3f A, peak=%.3f A, cal_limit=%.3f A\r\n",
      (unsigned long)trip_stage, (unsigned long)trip_elapsed, (double)requested_voltage,
      (double)trip_current[0], (double)trip_current[1], (double)trip_current[2], (double)trip_current[3],
      (double)peak_current, (double)MOTOR_CONTROL_CAL_CURRENT_LIMIT_A);
}
void MotorCalibration_Init(void)
{
  calibrated = CalibrationStore_Load(&record); saved = calibrated;
  if (calibrated) printf("Calibration loaded: direction=%ld, offset=%.6f rad\r\n",
                         (long)record.direction, (double)record.offset);
  else printf("WARNING: no valid calibration for this motor; run 'cal start' manually. FOC blocked.\r\n");
}
static void PrintStatus(void)
{
  printf("Calibration: %s, stage=%u, stored=%s, direction=%ld, offset=%.6f rad, fault=%s\r\n",
    calibrated ? "VALID" : "UNCALIBRATED", (unsigned)stage, saved ? "yes" : "no",
    calibrated ? (long)record.direction : 0L, calibrated ? (double)record.offset : 0.0,
    fault ? fault : "none");
}
/* 往復移動量を検証してからoffsetを求める。通電処理から独立させて検査可能にする。 */
static bool Solve(float first, float far, float returned, CalibrationRecord *result)
{
  if (!isfinite(first) || !isfinite(far) || !isfinite(returned)) return false;
  float expected = TWO_PI/MOTOR_CONTROL_POLE_PAIRS;
  float travel = far-first;
  if (fabsf(fabsf(travel)-expected) > expected*MOTOR_CONTROL_CAL_MOTION_TOLERANCE ||
      fabsf(returned-first)*MOTOR_CONTROL_POLE_PAIRS > MOTOR_CONTROL_CAL_POSITION_TOLERANCE_RAD)
    return false;
  int32_t order = travel > 0.0f ? 1 : -1;
  float equivalent_end = far-(float)order*expected;
  float offset = VoltageVector_Wrap((float)order*MOTOR_CONTROL_POLE_PAIRS*(first+returned+equivalent_end)/3.0f);
  CalibrationStore_Make(result,order,offset);
  return CalibrationStore_Valid(result);
}
/* 実機上で同じ演算コードを検査する。PWM/Flashには一切書かない。 */
static void SelfTest(void)
{
  unsigned failures = 0, checks = 0;
  float d[3];
  for (unsigned i=0; i<72; i++) {
    float angle = (float)i*TWO_PI/72.0f;
    for (unsigned j=0; j<3; j++) {
      float vd = j == 0 ? 0.0f : 1.0f, vq = j == 2 ? 2.0f : 0.0f;
      bool ok = VoltageVector_Compute(angle,vd,vq,24.0f,3.0f,0.05f,d);
      float a = (2.0f*d[0]-d[1]-d[2])*24.0f/3.0f;
      float b = (d[1]-d[2])*24.0f/1.732050808f;
      checks++;
      if (!ok || fabsf(a*cosf(angle)+b*sinf(angle)-vd)>0.0001f ||
          fabsf(-a*sinf(angle)+b*cosf(angle)-vq)>0.0001f) failures++;
    }
    bool ok = VoltageVector_Compute(angle,100.0f,-100.0f,6.0f,3.0f,0.05f,d);
    float a=(2*d[0]-d[1]-d[2])*2.0f, b=(d[1]-d[2])*6.0f/1.732050808f;
    checks++;
    if (!ok || fabsf(hypotf(a,b)-3.0f)>0.0001f ||
        d[0]<0.05f || d[1]<0.05f || d[2]<0.05f ||
        d[0]>0.95f || d[1]>0.95f || d[2]>0.95f) failures++;
  }
  checks += 3;
  if (VoltageVector_Compute(NAN,0,0,24,3,0.05f,d)) failures++;
  if (VoltageVector_Compute(0,0,0,0,3,0.05f,d)) failures++;
  if (VoltageVector_Compute(0,INFINITY,0,24,3,0.05f,d)) failures++;
  CalibrationRecord good, bad;
  CalibrationStore_Make(&good,-1,1.234f);
  checks++; if (!CalibrationStore_Valid(&good)) failures++;
  for (unsigned bit=0; bit<sizeof(good)*8; bit++) {
    bad = good; ((unsigned char *)&bad)[bit/8] ^= 1U<<(bit%8);
    checks++; if (CalibrationStore_Valid(&bad)) failures++;
  }
  checks += 2;
  CalibrationStore_Make(&bad,0,1.0f); if (CalibrationStore_Valid(&bad)) failures++;
  CalibrationStore_Make(&bad,1,NAN); if (CalibrationStore_Valid(&bad)) failures++;
  for (int order=-1; order<=1; order+=2) {
    for (unsigned i=0; i<16; i++) {
      float first = (float)i*0.5f;
      checks++;
      if (!Solve(first,first+(float)order*TWO_PI/MOTOR_CONTROL_POLE_PAIRS,first,&bad) ||
          bad.direction != order || fabsf(remainderf(bad.offset-(float)order*MOTOR_CONTROL_POLE_PAIRS*first,TWO_PI)) > 0.0001f)
        failures++;
    }
  }
  checks += 4;
  if (Solve(1,1,1,&bad)) failures++; /* ロック/無移動 */
  if (Solve(1,1+0.5f*TWO_PI/MOTOR_CONTROL_POLE_PAIRS,1,&bad)) failures++; /* 極対数違い */
  if (Solve(1,1+TWO_PI/MOTOR_CONTROL_POLE_PAIRS,1.2f,&bad)) failures++; /* 戻り不良 */
  if (Solve(NAN,1,1,&bad)) failures++;
  printf("Calibration selftest: %u checks, %u failures (no PWM/Flash writes)\r\n",checks,failures);
}
bool MotorCalibration_ProcessCommand(const char *command)
{
  while (isspace((unsigned char)*command)) command++;
  if (strlen(command)<3 || tolower((unsigned char)command[0])!='c' ||
      tolower((unsigned char)command[1])!='a' || tolower((unsigned char)command[2])!='l' ||
      (command[3] && !isspace((unsigned char)command[3]))) return false;
  const char *arg=command+3; while (isspace((unsigned char)*arg)) arg++;
  if (Same(arg,"status") || !*arg) PrintStatus();
  else if (Same(arg,"test")) {
    if (MotorCalibration_IsActive() || !MotorControl_IsStopped() || CurrentSense_IsBusy())
      printf("Selftest requires stopped PWM and ADC\r\n");
    else SelfTest();
  } else if (Same(arg,"stop")) {
    if (MotorCalibration_IsActive()) { MotorCalibration_TripISR("user stop"); Finish("aborted"); }
  } else if (Same(arg,"save")) {
    if (!calibrated || MotorCalibration_IsActive() || !MotorControl_IsStopped() || CurrentSense_IsBusy()) {
      printf("Save requires successful calibration and stopped PWM/ADC log\r\n"); return true;
    }
    printf("Saving calibration; wait for completion before sending commands\r\n");
    if (Console_Flush(1000U) != HAL_OK) { printf("Save cancelled: UART busy\r\n"); return true; }
    AS5047P_Pause();
    saved = CalibrationStore_Save(&record);
    AS5047P_Resume();
    printf("Calibration flash %s\r\n", saved ? "saved and verified" : "FAILED; RAM result only");
  } else if (Same(arg,"start")) {
    if (MotorCalibration_IsActive() || !MotorControl_IsStopped() || CurrentSense_IsBusy()) {
      printf("Send stop and adc stop, wait for idle, then cal start\r\n"); return true;
    }
    AS5047P_Sample sample; BusVoltageSample vm;
    if (!AS5047P_GetSample(&sample) || !BusVoltage_GetSample(&vm) || vm.overvoltage ||
        vm.volts < MOTOR_CONTROL_VM_MIN_VOLTS) {
      printf("Calibration blocked: invalid encoder or VM\r\n"); return true;
    }
    if (!isfinite(MOTOR_CONTROL_CAL_VOLTAGE) || MOTOR_CONTROL_CAL_VOLTAGE <= 0.0f ||
        MOTOR_CONTROL_CAL_VOLTAGE > MOTOR_CONTROL_VOLTAGE_LIMIT ||
        !isfinite(MOTOR_CONTROL_VOLTAGE_LIMIT) || MOTOR_CONTROL_VOLTAGE_LIMIT <= 0.0f ||
        !isfinite(MOTOR_CONTROL_PWM_MARGIN) || MOTOR_CONTROL_PWM_MARGIN < 0.05f || MOTOR_CONTROL_PWM_MARGIN >= 0.5f ||
        /* 校正閾値は独立。通常運転閾値より大きくても有効とする。 */
        !isfinite(MOTOR_CONTROL_CAL_CURRENT_LIMIT_A) || MOTOR_CONTROL_CAL_CURRENT_LIMIT_A <= 0.0f ||
        !isfinite(MOTOR_CONTROL_KV_RPM_PER_VOLT) || MOTOR_CONTROL_KV_RPM_PER_VOLT <= 0.0f ||
        MOTOR_CONTROL_CAL_RAMP_MS == 0U || MOTOR_CONTROL_CAL_SWEEP_MS == 0U ||
        MOTOR_CONTROL_CAL_HOLD_MS < 300U || MOTOR_CONTROL_POLE_PAIRS == 0U) {
      printf("Calibration blocked: invalid config\r\n"); return true;
    }
    /* 新しい校正の途中で失敗したら、古いRAM結果に戻して運転を許可しない。 */
    calibrated = false; saved = false; fault = NULL; energized = false;
    trip_captured = false; peak_current = requested_voltage = 0.0f;
    for (unsigned i=0; i<4; i++) observed_current[i] = 0.0f;
    previous_angle = position = sample.mechanical_rad; sequence = sample.sequence;
    task_ms = HAL_GetTick(); heartbeat = task_ms;
    Enter(CHECK_STILL, task_ms);
    printf("Manual calibration started: free rotor required, max %.2f V / %.2f A; stop aborts\r\n",
           (double)MOTOR_CONTROL_CAL_VOLTAGE, (double)MOTOR_CONTROL_CAL_CURRENT_LIMIT_A);
  } else printf("Usage: cal start | cal status | cal stop | cal save | cal test\r\n");
  return true;
}
void MotorCalibration_Task(void)
{
  if (!MotorCalibration_IsActive()) return;
  if (fault) { Finish(fault); return; }
  if (energized && !MotorControl_IsVoltageMode()) { Finish("aborted by stop"); return; }
  uint32_t now = HAL_GetTick();
  if (now == task_ms) return;
  task_ms = now;
  AS5047P_Sample sample; BusVoltageSample vm;
  if (!AS5047P_GetSample(&sample) || !BusVoltage_GetSample(&vm) || vm.overvoltage ||
      vm.volts < MOTOR_CONTROL_VM_MIN_VOLTS) {
    MotorCalibration_TripISR("invalid encoder or VM"); Finish(fault); return;
  }
  heartbeat = now;
  bool fresh = sample.sequence != sequence;
  if (fresh) {
    /* 0/2piの折り返しを跨いで機械角を積算。1msあたり半回転以上は想定しない。 */
    float delta = sample.mechanical_rad - previous_angle;
    if (delta > PI) delta -= TWO_PI;
    if (delta < -PI) delta += TWO_PI;
    position += delta; previous_angle = sample.mechanical_rad; sequence = sample.sequence;
  }
  uint32_t elapsed = now-stage_ms;
  float angle = 0.0f, volts = MOTOR_CONTROL_CAL_VOLTAGE;
  bool holding = stage == CHECK_STILL || stage == HOLD_ZERO || stage == HOLD_END || stage == HOLD_RETURN || stage == HOLD_APPROACH;
  /* holdの最後200msを平均。移動中や振動中の一発読みでoffsetを確定しない。 */
  if (holding && fresh && elapsed + 200U >= MOTOR_CONTROL_CAL_HOLD_MS) {
    sum += position; count++;
    minimum = fminf(minimum,position); maximum = fmaxf(maximum,position);
  }
  if (holding && elapsed >= MOTOR_CONTROL_CAL_HOLD_MS) {
    /* 保持終了点を比較し、位置依存誤差と往復の位置ずれを切り分ける。
     * 電流は直近1組のスナップショット。平均角は最後200msの値。 */
    float currents[4];
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    for (unsigned i=0; i<4; i++) currents[i] = observed_current[i];
    __set_PRIMASK(mask);
    printf("Cal endpoint: stage=%u, mech=%.6f rad, span_e=%.6f rad, n=%u, "
           "vd=%.3f V, U1=%.3f V=%.3f U2=%.3f W=%.3f A\r\n",
           (unsigned)stage, (double)(count ? sum/(float)count : position),
           (double)((maximum-minimum)*MOTOR_CONTROL_POLE_PAIRS), count,
           (double)(energized ? requested_voltage : 0.0f),
           (double)currents[0], (double)currents[1], (double)currents[2], (double)currents[3]);
    if (count < 20U || (maximum-minimum)*MOTOR_CONTROL_POLE_PAIRS > MOTOR_CONTROL_CAL_POSITION_TOLERANCE_RAD) {
      MotorCalibration_TripISR("rotor not settled"); Finish(fault); return;
    }
    float mean = sum/(float)count;
    if (stage == CHECK_STILL) {
      if (MotorControl_StartVoltage() != HAL_OK) { Finish("PWM start failed"); return; }
      current_ms = heartbeat = HAL_GetTick();
      if (CurrentSense_BeginControl() != HAL_OK) { Finish("ADC start failed"); return; }
      energized = true; Enter(RAMP,HAL_GetTick());
      return; /* 最初はゼロ電圧。次のtaskからramp。 */
    } else if (stage == HOLD_ZERO) {
      /* 静止状態からの引き込みは進入方向が不定。復路と同じ負方向から
       * 1電気回転かけて整列し直し、摩擦等による停止位置の差を減らす。
       * この間も電流/角度/VMの監視とstop受付は通常の校正と同じ。 */
      approach_start = mean; Enter(APPROACH,now);
    } else if (stage == HOLD_APPROACH) {
      float expected = TWO_PI/MOTOR_CONTROL_POLE_PAIRS;
      if (fabsf(fabsf(mean-approach_start)-expected) > expected*MOTOR_CONTROL_CAL_MOTION_TOLERANCE) {
        MotorCalibration_TripISR("prealignment travel mismatch"); Finish(fault); return;
      }
      origin = mean; Enter(FORWARD,now);
    } else if (stage == HOLD_END) {
      end_position = mean;
      float travel = end_position-origin;
      float expected = TWO_PI/MOTOR_CONTROL_POLE_PAIRS;
      if (fabsf(fabsf(travel)-expected) > expected*MOTOR_CONTROL_CAL_MOTION_TOLERANCE) {
        MotorCalibration_TripISR("travel/pole-pair mismatch"); Finish(fault); return;
      }
      Enter(BACKWARD,now);
    } else {
      if (fabsf(mean-origin)*MOTOR_CONTROL_POLE_PAIRS > MOTOR_CONTROL_CAL_POSITION_TOLERANCE_RAD) {
        printf("Calibration return error: %.6f electrical rad (limit %.6f)\r\n",
               (double)((mean-origin)*MOTOR_CONTROL_POLE_PAIRS),
               (double)MOTOR_CONTROL_CAL_POSITION_TOLERANCE_RAD);
        MotorCalibration_TripISR("return position mismatch"); Finish(fault); return;
      }
      calibrated = Solve(origin,end_position,mean,&record);
      Finish(calibrated ? "complete (RAM); send cal save after ADC idle" : "invalid result");
      PrintStatus(); return;
    }
    elapsed = 0;
  }
  if (stage == CHECK_STILL) return;
  if (stage == RAMP) {
    float fraction = fminf(1.0f,(float)elapsed/MOTOR_CONTROL_CAL_RAMP_MS);
    volts *= fraction;
    if (elapsed >= MOTOR_CONTROL_CAL_RAMP_MS) Enter(HOLD_ZERO,now);
  } else if (stage == APPROACH) {
    angle = -TWO_PI*fminf(1.0f,(float)elapsed/MOTOR_CONTROL_CAL_SWEEP_MS);
    if (elapsed >= MOTOR_CONTROL_CAL_SWEEP_MS) Enter(HOLD_APPROACH,now);
  } else if (stage == HOLD_APPROACH) angle = -TWO_PI;
  else if (stage == FORWARD) {
    angle = TWO_PI*fminf(1.0f,(float)elapsed/MOTOR_CONTROL_CAL_SWEEP_MS);
    if (elapsed >= MOTOR_CONTROL_CAL_SWEEP_MS) Enter(HOLD_END,now);
  } else if (stage == HOLD_END) angle = TWO_PI;
  else if (stage == BACKWARD) {
    angle = TWO_PI*(1.0f-fminf(1.0f,(float)elapsed/MOTOR_CONTROL_CAL_SWEEP_MS));
    if (elapsed >= MOTOR_CONTROL_CAL_SWEEP_MS) Enter(HOLD_RETURN,now);
  }
  requested_voltage = volts;
  if (!MotorControl_SetVoltage(angle,volts,0.0f,vm.volts)) {
    MotorCalibration_TripISR("voltage output rejected"); Finish(fault);
  }
}
