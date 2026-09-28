#include "motor_control.h"
#include "as5047p.h"
#include "voltage_vector.h"
#include "motor_calibration.h"
#include "foc_voltage.h"
#include "stspin32g4.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define MOTOR_CONTROL_OUTPUT_ENABLE_MASK                                  \
  (TIM_CCER_CC1E | TIM_CCER_CC1NE | TIM_CCER_CC2E | TIM_CCER_CC2NE |    \
   TIM_CCER_CC3E | TIM_CCER_CC3NE)

#define MOTOR_CONTROL_PHASE_U_OUTPUTS (TIM_CCER_CC1E | TIM_CCER_CC1NE)
#define MOTOR_CONTROL_PHASE_V_OUTPUTS (TIM_CCER_CC2E | TIM_CCER_CC2NE)
#define MOTOR_CONTROL_PHASE_W_OUTPUTS (TIM_CCER_CC3E | TIM_CCER_CC3NE)

#if (MOTOR_CONTROL_BOOTSTRAP_CHARGE_US < 500U) || \
    (MOTOR_CONTROL_BOOTSTRAP_CHARGE_US > 1000U)
#error "Bootstrap charge time must be between 500 and 1000 us"
#endif

#if (MOTOR_CONTROL_POLE_PAIRS == 0U)
#error "MOTOR_CONTROL_POLE_PAIRS must be greater than zero"
#endif

#if ((MOTOR_CONTROL_ALIGNMENT_DUTY_X10 == 0U) || \
     (MOTOR_CONTROL_ALIGNMENT_DUTY_X10 >= 1000U))
#error "MOTOR_CONTROL_ALIGNMENT_DUTY_X10 must be between 1 and 999"
#endif

#if ((MOTOR_CONTROL_RUN_START_DUTY_X10 == 0U) || \
     (MOTOR_CONTROL_RUN_START_DUTY_X10 > MOTOR_CONTROL_MAX_DUTY_X10))
#error "MOTOR_CONTROL_RUN_START_DUTY_X10 must be between 1 and MAX_DUTY_X10"
#endif

#if ((MOTOR_CONTROL_MAX_DUTY_X10 == 0U) || \
     (MOTOR_CONTROL_MAX_DUTY_X10 >= 1000U))
#error "MOTOR_CONTROL_MAX_DUTY_X10 must be between 1 and 999"
#endif

#if (MOTOR_CONTROL_DUTY_RISE_RPM == 0U)
#error "MOTOR_CONTROL_DUTY_RISE_RPM must be greater than zero"
#endif

#if (MOTOR_CONTROL_MIN_TARGET_RPM < MOTOR_CONTROL_START_RPM)
#error "MOTOR_CONTROL_MIN_TARGET_RPM must not be below START_RPM"
#endif

#if (MOTOR_CONTROL_MAX_TARGET_RPM < MOTOR_CONTROL_MIN_TARGET_RPM)
#error "MOTOR_CONTROL_MAX_TARGET_RPM must not be below MIN_TARGET_RPM"
#endif

typedef enum
{
  MOTOR_CONTROL_MODE_STOPPED = 0,
  MOTOR_CONTROL_MODE_MANUAL,
  MOTOR_CONTROL_MODE_VOLTAGE,
  MOTOR_CONTROL_MODE_SIX_STEP_ALIGNMENT,
  MOTOR_CONTROL_MODE_SIX_STEP_RAMP,
  MOTOR_CONTROL_MODE_SIX_STEP_RUNNING
} MotorControlMode;

typedef enum
{
  MOTOR_CONTROL_DIRECTION_CW = 0,
  MOTOR_CONTROL_DIRECTION_CCW
} MotorControlDirection;

static TIM_HandleTypeDef *motor_timer;
static I2C_HandleTypeDef *gate_driver_i2c;
static MotorControlPhase selected_phase = MOTOR_CONTROL_PHASE_U;
static float selected_offset_percent;
static volatile bool outputs_enabled;
static volatile uint32_t voltage_compare[3];
static volatile uint32_t foc_adc_began, foc_adc_phase, foc_update_cnt, foc_adc_cnt;
static volatile bool foc_update_down, foc_adc_down;
static volatile MotorControlMode motor_mode = MOTOR_CONTROL_MODE_STOPPED;
static volatile uint8_t current_sector;
static volatile uint32_t reference_rpm;
static volatile uint32_t current_duty_x10;
static MotorControlDirection six_step_direction;
static uint32_t target_rpm;
static uint32_t control_tick_hz;
static uint32_t alignment_ticks_remaining;
static uint32_t ramp_elapsed_ticks;
static uint32_t ramp_total_ticks;
static uint32_t commutation_accumulator;

static const char *MotorControl_SkipSpaces(const char *text)
{
  while ((*text != '\0') && (isspace((unsigned char)*text) != 0))
  {
    text++;
  }
  return text;
}

static bool MotorControl_IsCommand(const char *text, const char *expected)
{
  while ((*text != '\0') && (*expected != '\0'))
  {
    if (tolower((unsigned char)*text) != tolower((unsigned char)*expected))
    {
      return false;
    }
    text++;
    expected++;
  }

  text = MotorControl_SkipSpaces(text);
  return ((*text == '\0') && (*expected == '\0'));
}

static const char *MotorControl_PhaseName(MotorControlPhase phase)
{
  static const char *const phase_names[] = {"U", "V", "W"};
  return phase_names[(unsigned int)phase];
}

static const char *MotorControl_DirectionName(MotorControlDirection direction)
{
  return (direction == MOTOR_CONTROL_DIRECTION_CW) ? "CW" : "CCW";
}

static const char *MotorControl_SixStepStageName(MotorControlMode mode)
{
  if (mode == MOTOR_CONTROL_MODE_SIX_STEP_ALIGNMENT)
  {
    return "alignment";
  }
  if (mode == MOTOR_CONTROL_MODE_SIX_STEP_RAMP)
  {
    return "ramp";
  }
  return "running";
}

static bool MotorControl_IsSixStepMode(MotorControlMode mode)
{
  return ((mode == MOTOR_CONTROL_MODE_SIX_STEP_ALIGNMENT) ||
          (mode == MOTOR_CONTROL_MODE_SIX_STEP_RAMP) ||
          (mode == MOTOR_CONTROL_MODE_SIX_STEP_RUNNING));
}

static uint32_t MotorControl_GetControlTickHz(void)
{
  RCC_ClkInitTypeDef clock_config;
  uint32_t flash_latency;
  uint32_t timer_clock_hz = HAL_RCC_GetPCLK2Freq();
  const uint32_t timer_divider = motor_timer->Instance->PSC + 1U;
  const uint32_t half_period_counts = motor_timer->Instance->ARR + 1U;

  HAL_RCC_GetClockConfig(&clock_config, &flash_latency);
  if (clock_config.APB2CLKDivider != RCC_HCLK_DIV1)
  {
    timer_clock_hz *= 2U;
  }

  return timer_clock_hz / (2U * timer_divider * half_period_counts);
}

static uint32_t MotorControl_MillisecondsToTicks(uint32_t milliseconds)
{
  return (uint32_t)((((uint64_t)control_tick_hz * milliseconds) + 999U) /
                    1000U);
}

static uint32_t MotorControl_MidpointCompare(void)
{
  return (__HAL_TIM_GET_AUTORELOAD(motor_timer) + 1U) / 2U;
}

static void MotorControl_WriteMidpoint(void)
{
  const uint32_t midpoint = MotorControl_MidpointCompare();

  __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_1, midpoint);
  __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_2, midpoint);
  __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_3, midpoint);
}

static void MotorControl_DelayMicroseconds(uint32_t microseconds)
{
  const uint32_t cycles = (uint32_t)
    (((uint64_t)SystemCoreClock * microseconds) / 1000000U);
  const uint32_t start_cycles = DWT->CYCCNT;

  while ((uint32_t)(DWT->CYCCNT - start_cycles) < cycles)
  {
    /* Interrupts remain enabled; the motor mode stays STOPPED. */
  }
}

/* 停止中のタイマの開始位相を揃える。FOCは頂点から下降、それ以外は底から上昇。
 * RCR=1をUGで再ロードし、開始位置へ1往復して最初の更新イベントを発生させる。
 * 中央揃え動作中のDIRは読み取り専用のため、一時的にCMSを解除して方向を戻す。
 * これは再始動時の位相合わせのみ。CubeMXが指定したCMS/RCR/PSC/ARRは維持する。
 * 呼出し側はCEN=0、全モーター出力OFFを保証すること。 */
HAL_StatusTypeDef MotorControl_ResetTimerPhase(TIM_HandleTypeDef *timer)
{
  uint32_t mask=__get_PRIMASK(); __disable_irq();
  if ((timer->Instance->CR1 & TIM_CR1_CEN) ||
      (timer->Instance->CCER & MOTOR_CONTROL_OUTPUT_ENABLE_MASK)) {
    __set_PRIMASK(mask);
    return HAL_ERROR;
  }
  /* FOCだけは頂点から下降して開始する。UGでRCR=1をロードし、最初の底を
   * スキップして次の頂点でUEVを発生させる。校正/六ステップ/ADC単独は従来の底基準。 */
  bool peak_start=FocVoltage_IsActive();
  uint32_t mode=timer->Instance->CR1 & TIM_CR1_CMS;
  CLEAR_BIT(timer->Instance->CR1, TIM_CR1_CMS);
  MODIFY_REG(timer->Instance->CR1, TIM_CR1_DIR, peak_start ? TIM_CR1_DIR : 0U);
  SET_BIT(timer->Instance->CR1, mode);
  __HAL_TIM_SET_COUNTER(timer, 0U);
  /* HALのイベント生成APIでCCR/ARRと反復カウンタを初期化する。 */
  HAL_StatusTypeDef result=HAL_TIM_GenerateEvent(timer, TIM_EVENTSOURCE_UPDATE);
  /* UGがCNTを初期化するため、開始位置はUGの後に指定する。 */
  __HAL_TIM_SET_COUNTER(timer, peak_start ? __HAL_TIM_GET_AUTORELOAD(timer) : 0U);
  __HAL_TIM_CLEAR_FLAG(timer, TIM_FLAG_UPDATE | TIM_FLAG_CC4);
  __set_PRIMASK(mask);
  return result;
}

static HAL_StatusTypeDef MotorControl_PreparePwmStart(void)
{
  TIM_TypeDef *tim = motor_timer->Instance;
  STSPIN32G4_FaultReport report;
  HAL_StatusTypeDef result;
  uint8_t status;
  GPIO_PinState nfault;
  uint32_t saved_ccmr1;
  uint32_t saved_ccmr2;

  MotorControl_Stop();

  /* An existing latch can block even the low-side charging pulse. */
  result = STSPIN32G4_CheckAndClearFaults(gate_driver_i2c, &report);
  if (result != HAL_OK)
  {
    goto i2c_error;
  }
  if (STSPIN32G4_StatusHasFault(report.after_clear) ||
      (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_15) == GPIO_PIN_RESET))
  {
    printf("PWM start blocked before bootstrap: STATUS=0x%02X\r\n",
           report.after_clear);
    return HAL_ERROR;
  }

  /* DWT gives a sub-ms pulse without HAL_Delay's 1 ms tick rounding.
     Do not reset CYCCNT: other timing users may be using it. */
  SET_BIT(CoreDebug->DEMCR, CoreDebug_DEMCR_TRCENA_Msk);
  SET_BIT(DWT->CTRL, DWT_CTRL_CYCCNTENA_Msk);
  __DSB();
  __ISB();

  saved_ccmr1 = tim->CCMR1;
  saved_ccmr2 = tim->CCMR2;
  /* Active-high CHx/CHxN: forced-low OCREF drives high sides low and
     complementary low sides high, with the configured dead time. */
  MODIFY_REG(tim->CCMR1, TIM_CCMR1_OC1M | TIM_CCMR1_OC2M,
             TIM_OCMODE_FORCED_INACTIVE | (TIM_OCMODE_FORCED_INACTIVE << 8U));
  MODIFY_REG(tim->CCMR2, TIM_CCMR2_OC3M, TIM_OCMODE_FORCED_INACTIVE);
  if (MotorControl_ResetTimerPhase(motor_timer) != HAL_OK) return HAL_ERROR;
  SET_BIT(tim->CCER, MOTOR_CONTROL_OUTPUT_ENABLE_MASK);
  SET_BIT(tim->CR1, TIM_CR1_CEN);
  SET_BIT(tim->BDTR, TIM_BDTR_MOE);
  MotorControl_DelayMicroseconds(MOTOR_CONTROL_BOOTSTRAP_CHARGE_US);

  /* All six outputs OFF before restoring PWM modes or clearing faults. */
  MotorControl_Stop();
  tim->CCMR1 = saved_ccmr1;
  tim->CCMR2 = saved_ccmr2;
  result = STSPIN32G4_ClearFaults(gate_driver_i2c);
  if (result != HAL_OK)
  {
    goto i2c_error;
  }
  /* Datasheet tFAULT,reset = 160 us. Avoid a full SysTick delay here
     to minimize the interval between bootstrap charging and PWM. */
  MotorControl_DelayMicroseconds(200U);
  result = STSPIN32G4_ReadStatus(gate_driver_i2c, &status);
  if (result != HAL_OK)
  {
    goto i2c_error;
  }
  nfault = HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_15);
  if (STSPIN32G4_StatusHasFault(status) || (nfault == GPIO_PIN_RESET))
  {
    printf("PWM start blocked after bootstrap/CLEAR: STATUS=0x%02X, nFAULT=%u\r\n",
           status, (unsigned int)(nfault == GPIO_PIN_SET));
    return HAL_ERROR;
  }
  return HAL_OK;

i2c_error:
  printf("PWM start blocked: gate-driver I2C status=%d, error=0x%08lX\r\n",
         (int)result, (unsigned long)HAL_I2C_GetError(gate_driver_i2c));
  return result;
}

static HAL_StatusTypeDef MotorControl_StartAtMidpoint(void)
{
  TIM_TypeDef *tim = motor_timer->Instance;
  uint32_t interrupt_state;
  HAL_StatusTypeDef result = MotorControl_PreparePwmStart();

  if (result != HAL_OK)
  {
    return result;
  }
  interrupt_state = __get_PRIMASK();

  __disable_irq();

  /* Keep every half bridge disabled until all preload values are ready. */
  CLEAR_BIT(tim->BDTR, TIM_BDTR_MOE);
  CLEAR_BIT(tim->CR1, TIM_CR1_CEN);
  CLEAR_BIT(tim->CCER, MOTOR_CONTROL_OUTPUT_ENABLE_MASK);

  MotorControl_WriteMidpoint();
  result=MotorControl_ResetTimerPhase(motor_timer);
  if (result != HAL_OK) {
    __set_PRIMASK(interrupt_state);
    return result;
  }

  /* Enable all six pins together, then start the counter and main output. */
  SET_BIT(tim->CCER, MOTOR_CONTROL_OUTPUT_ENABLE_MASK);
  SET_BIT(tim->CR1, TIM_CR1_CEN);
  SET_BIT(tim->BDTR, TIM_BDTR_MOE);

  current_sector = 0U;
  reference_rpm = 0U;
  current_duty_x10 = 0U;
  motor_mode = MOTOR_CONTROL_MODE_MANUAL;

  if (interrupt_state == 0U)
  {
    __enable_irq();
  }

  selected_offset_percent = 0.0f;
  outputs_enabled = true;
  return HAL_OK;
}

static uint32_t MotorControl_RunDutyX10(uint32_t rpm)
{
  uint64_t duty_x10 = MOTOR_CONTROL_RUN_START_DUTY_X10;

  if (rpm > MOTOR_CONTROL_START_RPM)
  {
    const uint32_t rpm_above_start = rpm - MOTOR_CONTROL_START_RPM;

    duty_x10 += (((uint64_t)rpm_above_start *
                  MOTOR_CONTROL_DUTY_RISE_X10) +
                 (MOTOR_CONTROL_DUTY_RISE_RPM / 2U)) /
                MOTOR_CONTROL_DUTY_RISE_RPM;
  }

  if (duty_x10 > MOTOR_CONTROL_MAX_DUTY_X10)
  {
    duty_x10 = MOTOR_CONTROL_MAX_DUTY_X10;
  }

  return (uint32_t)duty_x10;
}

static uint32_t MotorControl_SixStepDutyX10(void)
{
  if (motor_mode == MOTOR_CONTROL_MODE_SIX_STEP_ALIGNMENT)
  {
    return MOTOR_CONTROL_ALIGNMENT_DUTY_X10;
  }

  return MotorControl_RunDutyX10(reference_rpm);
}

static uint32_t MotorControl_SixStepCompare(uint32_t duty_x10)
{
  uint32_t compare =
    (uint32_t)((((uint64_t)(__HAL_TIM_GET_AUTORELOAD(motor_timer) + 1U) *
                 duty_x10) + 500U) / 1000U);

  if (compare == 0U)
  {
    compare = 1U;
  }
  return compare;
}

static void MotorControl_ApplySixStepSector(uint8_t sector)
{
  TIM_TypeDef *tim = motor_timer->Instance;
  const uint32_t duty_x10 = MotorControl_SixStepDutyX10();
  const uint32_t pwm_compare = MotorControl_SixStepCompare(duty_x10);
  /* Center both active phases around 50%, preserving the compare difference
   * even when it is odd. Each phase uses complementary high/low-side PWM. */
  const uint32_t low_compare = MotorControl_MidpointCompare() - (pwm_compare / 2U);
  const uint32_t high_compare = low_compare + pwm_compare;
  uint32_t output_mask;
  uint32_t u_compare = 0U;
  uint32_t v_compare = 0U;
  uint32_t w_compare = 0U;
  uint32_t interrupt_state;

  switch (sector)
  {
    case 1U: /* U positive, V negative, W floating. */
      u_compare = high_compare;
      v_compare = low_compare;
      output_mask = MOTOR_CONTROL_PHASE_U_OUTPUTS |
                    MOTOR_CONTROL_PHASE_V_OUTPUTS;
      break;

    case 2U: /* U positive, W negative, V floating. */
      u_compare = high_compare;
      w_compare = low_compare;
      output_mask = MOTOR_CONTROL_PHASE_U_OUTPUTS |
                    MOTOR_CONTROL_PHASE_W_OUTPUTS;
      break;

    case 3U: /* V positive, W negative, U floating. */
      v_compare = high_compare;
      w_compare = low_compare;
      output_mask = MOTOR_CONTROL_PHASE_V_OUTPUTS |
                    MOTOR_CONTROL_PHASE_W_OUTPUTS;
      break;

    case 4U: /* V positive, U negative, W floating. */
      v_compare = high_compare;
      u_compare = low_compare;
      output_mask = MOTOR_CONTROL_PHASE_V_OUTPUTS |
                    MOTOR_CONTROL_PHASE_U_OUTPUTS;
      break;

    case 5U: /* W positive, U negative, V floating. */
      w_compare = high_compare;
      u_compare = low_compare;
      output_mask = MOTOR_CONTROL_PHASE_W_OUTPUTS |
                    MOTOR_CONTROL_PHASE_U_OUTPUTS;
      break;

    case 6U: /* W positive, V negative, U floating. */
      w_compare = high_compare;
      v_compare = low_compare;
      output_mask = MOTOR_CONTROL_PHASE_W_OUTPUTS |
                    MOTOR_CONTROL_PHASE_V_OUTPUTS;
      break;

    default:
      return;
  }

  interrupt_state = __get_PRIMASK();
  __disable_irq();

  /* Blank all phases while preload values and output enables are changed. */
  CLEAR_BIT(tim->BDTR, TIM_BDTR_MOE);
  CLEAR_BIT(tim->CCER, MOTOR_CONTROL_OUTPUT_ENABLE_MASK);

  __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_1, u_compare);
  __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_2, v_compare);
  __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_3, w_compare);
  tim->EGR = TIM_EGR_UG;
  __HAL_TIM_CLEAR_FLAG(motor_timer, TIM_FLAG_UPDATE);

  SET_BIT(tim->CCER, output_mask);
  SET_BIT(tim->CR1, TIM_CR1_CEN);
  SET_BIT(tim->BDTR, TIM_BDTR_MOE);
  current_sector = sector;
  current_duty_x10 = duty_x10;

  if (interrupt_state == 0U)
  {
    __enable_irq();
  }
}

static void MotorControl_AdvanceSector(void)
{
  uint8_t next_sector = current_sector;

  if (six_step_direction == MOTOR_CONTROL_DIRECTION_CW)
  {
    next_sector = (next_sector >= 6U) ? 1U : (uint8_t)(next_sector + 1U);
  }
  else
  {
    next_sector = (next_sector <= 1U) ? 6U : (uint8_t)(next_sector - 1U);
  }

  MotorControl_ApplySixStepSector(next_sector);
}

static void MotorControl_SixStepTick(void)
{
  uint32_t phase_threshold;

  if (motor_mode == MOTOR_CONTROL_MODE_SIX_STEP_ALIGNMENT)
  {
    if (alignment_ticks_remaining > 0U)
    {
      alignment_ticks_remaining--;
      return;
    }

    reference_rpm = MOTOR_CONTROL_START_RPM;
    ramp_elapsed_ticks = 0U;
    commutation_accumulator = 0U;
    motor_mode = (target_rpm == MOTOR_CONTROL_START_RPM) ?
                 MOTOR_CONTROL_MODE_SIX_STEP_RUNNING :
                 MOTOR_CONTROL_MODE_SIX_STEP_RAMP;
  }

  if (motor_mode == MOTOR_CONTROL_MODE_SIX_STEP_RAMP)
  {
    if (ramp_elapsed_ticks < ramp_total_ticks)
    {
      const uint32_t rpm_delta = target_rpm - MOTOR_CONTROL_START_RPM;

      ramp_elapsed_ticks++;
      reference_rpm = MOTOR_CONTROL_START_RPM +
        (uint32_t)(((uint64_t)rpm_delta * ramp_elapsed_ticks) /
                   ramp_total_ticks);
    }
    else
    {
      reference_rpm = target_rpm;
      motor_mode = MOTOR_CONTROL_MODE_SIX_STEP_RUNNING;
    }
  }

  phase_threshold = control_tick_hz * 10U;
  commutation_accumulator += reference_rpm * MOTOR_CONTROL_POLE_PAIRS;
  if (commutation_accumulator >= phase_threshold)
  {
    commutation_accumulator -= phase_threshold;
    MotorControl_AdvanceSector();
  }
}

static void MotorControl_StartSixStep(MotorControlDirection direction,
                                      uint32_t requested_rpm)
{
  TIM_TypeDef *tim = motor_timer->Instance;
  const uint32_t target_duty_x10 = MotorControl_RunDutyX10(requested_rpm);
  uint32_t interrupt_state;

  if (MotorControl_PreparePwmStart() != HAL_OK)
  {
    return;
  }
  interrupt_state = __get_PRIMASK();

  __disable_irq();
  CLEAR_BIT(tim->BDTR, TIM_BDTR_MOE);
  CLEAR_BIT(tim->CR1, TIM_CR1_CEN);
  CLEAR_BIT(tim->CCER, MOTOR_CONTROL_OUTPUT_ENABLE_MASK);

  six_step_direction = direction;
  target_rpm = requested_rpm;
  reference_rpm = 0U;
  current_sector = 0U;
  alignment_ticks_remaining =
    MotorControl_MillisecondsToTicks(MOTOR_CONTROL_ALIGNMENT_TIME_MS);
  ramp_elapsed_ticks = 0U;
  ramp_total_ticks =
    MotorControl_MillisecondsToTicks(MOTOR_CONTROL_ACCELERATION_TIME_MS);
  commutation_accumulator = 0U;
  outputs_enabled = true;
  motor_mode = MOTOR_CONTROL_MODE_SIX_STEP_ALIGNMENT;
  __HAL_TIM_SET_COUNTER(motor_timer, 0U);

  if (interrupt_state == 0U)
  {
    __enable_irq();
  }

  MotorControl_ApplySixStepSector(1U);

  printf("Six-step started: direction=%s, target=%lu rpm, "
         "duty_start=%lu.%lu %%, duty_target=%lu.%lu %%, "
         "alignment=%u ms, ramp=%u ms\r\n",
         MotorControl_DirectionName(direction),
         (unsigned long)requested_rpm,
         (unsigned long)(MOTOR_CONTROL_RUN_START_DUTY_X10 / 10U),
         (unsigned long)(MOTOR_CONTROL_RUN_START_DUTY_X10 % 10U),
         (unsigned long)(target_duty_x10 / 10U),
         (unsigned long)(target_duty_x10 % 10U),
         (unsigned int)MOTOR_CONTROL_ALIGNMENT_TIME_MS,
         (unsigned int)MOTOR_CONTROL_ACCELERATION_TIME_MS);
}

static bool MotorControl_ParseRunCommand(const char *command)
{
  const char *argument;
  char *parse_end;
  unsigned long parsed_rpm;
  MotorControlDirection direction;

  if ((tolower((unsigned char)command[0]) != 'r') ||
      (tolower((unsigned char)command[1]) != 'u') ||
      (tolower((unsigned char)command[2]) != 'n') ||
      ((command[3] != '\0') &&
       (isspace((unsigned char)command[3]) == 0)))
  {
    return false;
  }

  argument = MotorControl_SkipSpaces(&command[3]);
  if ((tolower((unsigned char)argument[0]) == 'c') &&
      (tolower((unsigned char)argument[1]) == 'w') &&
      (isspace((unsigned char)argument[2]) != 0))
  {
    direction = MOTOR_CONTROL_DIRECTION_CW;
    argument = MotorControl_SkipSpaces(&argument[2]);
  }
  else if ((tolower((unsigned char)argument[0]) == 'c') &&
           (tolower((unsigned char)argument[1]) == 'c') &&
           (tolower((unsigned char)argument[2]) == 'w') &&
           (isspace((unsigned char)argument[3]) != 0))
  {
    direction = MOTOR_CONTROL_DIRECTION_CCW;
    argument = MotorControl_SkipSpaces(&argument[3]);
  }
  else
  {
    printf("Usage: run cw <rpm> or run ccw <rpm>\r\n");
    return true;
  }

  if (isdigit((unsigned char)*argument) == 0)
  {
    printf("Usage: run cw <rpm> or run ccw <rpm>\r\n");
    return true;
  }

  errno = 0;
  parsed_rpm = strtoul(argument, &parse_end, 10);
  parse_end = (char *)MotorControl_SkipSpaces(parse_end);
  if ((errno == ERANGE) || (parsed_rpm > UINT32_MAX) ||
      (*parse_end != '\0') ||
      (parsed_rpm < MOTOR_CONTROL_MIN_TARGET_RPM) ||
      (parsed_rpm > MOTOR_CONTROL_MAX_TARGET_RPM))
  {
    printf("RPM must be between %u and %u\r\n",
           (unsigned int)MOTOR_CONTROL_MIN_TARGET_RPM,
           (unsigned int)MOTOR_CONTROL_MAX_TARGET_RPM);
    return true;
  }

  MotorControl_StartSixStep(direction, (uint32_t)parsed_rpm);
  return true;
}

static bool MotorControl_ParseOffset(const char *text, float *offset_percent)
{
  char *parse_end;
  float parsed_value;

  text = MotorControl_SkipSpaces(text);
  errno = 0;
  parsed_value = strtof(text, &parse_end);

  if ((parse_end == text) || (errno == ERANGE) || !isfinite(parsed_value))
  {
    return false;
  }

  parse_end = (char *)MotorControl_SkipSpaces(parse_end);
  if (*parse_end != '\0')
  {
    return false;
  }

  if ((parsed_value < -MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT) ||
      (parsed_value > MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT))
  {
    printf("Offset must be between %.1f and +%.1f %%\r\n",
           -MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT,
           MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT);
    return false;
  }

  *offset_percent = parsed_value;
  return true;
}

static void MotorControl_ApplyOffset(MotorControlPhase phase, float offset_percent)
{
  const float timer_counts =
    (float)(__HAL_TIM_GET_AUTORELOAD(motor_timer) + 1U);
  const float compare_value =
    timer_counts * (0.5f + (offset_percent / 100.0f));
  const uint32_t rounded_compare =
    (uint32_t)(compare_value + ((compare_value >= 0.0f) ? 0.5f : -0.5f));

  /* CCR preload makes all three new values effective at the same update event. */
  MotorControl_WriteMidpoint();

  switch (phase)
  {
    case MOTOR_CONTROL_PHASE_U:
      __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_1, rounded_compare);
      break;

    case MOTOR_CONTROL_PHASE_V:
      __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_2, rounded_compare);
      break;

    case MOTOR_CONTROL_PHASE_W:
      __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_3, rounded_compare);
      break;

    default:
      return;
  }

  selected_phase = phase;
  selected_offset_percent = offset_percent;

  printf("PWM: %s=%.2f %%, others=50.00 %% (offset %+.2f %%)\r\n",
         MotorControl_PhaseName(phase),
         50.0f + offset_percent,
         offset_percent);
}

HAL_StatusTypeDef MotorControl_Init(TIM_HandleTypeDef *htim,
                                     I2C_HandleTypeDef *hi2c)
{
  if ((htim == NULL) || (hi2c == NULL) ||
      !IS_TIM_CCXN_INSTANCE(htim->Instance, TIM_CHANNEL_3) ||
      (MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT <= 0.0f) ||
      (MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT >= 50.0f) ||
      (MOTOR_CONTROL_MAX_TARGET_RPM < MOTOR_CONTROL_START_RPM) ||
      (MOTOR_CONTROL_ACCELERATION_TIME_MS == 0U))
  {
    return HAL_ERROR;
  }

  motor_timer = htim;
  gate_driver_i2c = hi2c;
  control_tick_hz = MotorControl_GetControlTickHz();
  if (control_tick_hz == 0U)
  {
    return HAL_ERROR;
  }

  selected_phase = MOTOR_CONTROL_PHASE_U;
  __HAL_TIM_CLEAR_FLAG(motor_timer, TIM_FLAG_UPDATE);
  __HAL_TIM_ENABLE_IT(motor_timer, TIM_IT_UPDATE);
  MotorControl_Stop();
  return HAL_OK;
}

bool MotorControl_ProcessCommand(const char *command)
{
  const char *argument;
  MotorControlPhase command_phase = selected_phase;
  float offset_percent;

  if ((motor_timer == NULL) || (command == NULL))
  {
    return false;
  }

  command = MotorControl_SkipSpaces(command);

  if ((MotorCalibration_IsActive() || FocVoltage_IsActive()) && !MotorControl_IsCommand(command, "stop") &&
      !MotorControl_IsCommand(command, "status")) {
    printf("Calibration/FOC active; send stop first\r\n"); return true;
  }
  if (MotorControl_ParseRunCommand(command))
  {
    return true;
  }

  if (MotorControl_IsCommand(command, "stop"))
  {
    MotorCalibration_TripISR("user stop");
    FocVoltage_TripISR("user stop");
    MotorControl_Stop();
    printf("PWM stopped\r\n");
    return true;
  }

  if (MotorControl_IsCommand(command, "start"))
  {
    if (MotorControl_StartAtMidpoint() == HAL_OK)
    {
      printf("PWM started: U=V=W=50.00 %%\r\n");
    }
    return true;
  }

  if (MotorControl_IsCommand(command, "mid"))
  {
    if (MotorControl_IsSixStepMode(motor_mode))
    {
      printf("Manual PWM is unavailable during six-step drive; send 'stop' "
             "or 'start' first\r\n");
      return true;
    }
    if (!outputs_enabled)
    {
      printf("PWM is stopped; send 'start' first\r\n");
      return false;
    }
    MotorControl_WriteMidpoint();
    selected_offset_percent = 0.0f;
    printf("PWM: U=V=W=50.00 %%\r\n");
    return true;
  }

  if (MotorControl_IsCommand(command, "status"))
  {
    if (MotorControl_IsSixStepMode(motor_mode))
    {
      printf("PWM: six-step, stage=%s, direction=%s, target=%lu rpm, "
             "reference=%lu rpm, sector=%u, duty=%lu.%lu %%\r\n",
             MotorControl_SixStepStageName(motor_mode),
             MotorControl_DirectionName(six_step_direction),
             (unsigned long)target_rpm,
             (unsigned long)reference_rpm,
             (unsigned int)current_sector,
             (unsigned long)(current_duty_x10 / 10U),
             (unsigned long)(current_duty_x10 % 10U));
    }
    else
    {
      printf("PWM: %s, mode=%s, selected=%s, offset=%+.2f %%\r\n",
             outputs_enabled ? "running" : "stopped",
             (motor_mode == MOTOR_CONTROL_MODE_VOLTAGE) ? (FocVoltage_IsActive() ? "foc-voltage" : "calibration") :
             (motor_mode == MOTOR_CONTROL_MODE_MANUAL) ? "manual" : "off",
             MotorControl_PhaseName(selected_phase),
             selected_offset_percent);
    }
    return true;
  }

  argument = command;
  if (((command[0] == 'u') || (command[0] == 'U') ||
       (command[0] == 'v') || (command[0] == 'V') ||
       (command[0] == 'w') || (command[0] == 'W')) &&
      (isspace((unsigned char)command[1]) != 0))
  {
    switch (tolower((unsigned char)command[0]))
    {
      case 'u': command_phase = MOTOR_CONTROL_PHASE_U; break;
      case 'v': command_phase = MOTOR_CONTROL_PHASE_V; break;
      case 'w': command_phase = MOTOR_CONTROL_PHASE_W; break;
      default: break;
    }
    argument = &command[1];
  }

  if (!MotorControl_ParseOffset(argument, &offset_percent))
  {
    printf("Invalid command. Use: <offset>, u/v/w <offset>, mid, stop, "
           "start, run cw/ccw <rpm>, status\r\n");
    return false;
  }

  if (MotorControl_IsSixStepMode(motor_mode))
  {
    printf("Manual PWM is unavailable during six-step drive; send 'stop' "
           "or 'start' first\r\n");
    return true;
  }

  if (!outputs_enabled)
  {
    printf("PWM is stopped; send 'start' first\r\n");
    return false;
  }

  MotorControl_ApplyOffset(command_phase, offset_percent);
  return true;
}

bool MotorControl_ProcessStopCommand(const char *command)
{
  if ((motor_timer == NULL) || (command == NULL))
  {
    return false;
  }

  command = MotorControl_SkipSpaces(command);
  if (!MotorControl_IsCommand(command, "stop"))
  {
    return false;
  }

  MotorCalibration_TripISR("user stop");
  FocVoltage_TripISR("user stop");
  MotorControl_Stop();
  printf("PWM stopped\r\n");
  return true;
}

void MotorControl_Stop(void)
{
  uint32_t interrupt_state;

  if (motor_timer == NULL)
  {
    return;
  }

  interrupt_state = __get_PRIMASK();
  __disable_irq();

  CLEAR_BIT(motor_timer->Instance->BDTR, TIM_BDTR_MOE);
  CLEAR_BIT(motor_timer->Instance->CR1, TIM_CR1_CEN);
  CLEAR_BIT(motor_timer->Instance->CCER, MOTOR_CONTROL_OUTPUT_ENABLE_MASK);

  current_sector = 0U;
  reference_rpm = 0U;
  current_duty_x10 = 0U;
  motor_mode = MOTOR_CONTROL_MODE_STOPPED;

  if (interrupt_state == 0U)
  {
    __enable_irq();
  }

  outputs_enabled = false;
}

uint8_t MotorControl_GetSector(void)
{
  return current_sector;
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim != motor_timer)
  {
    return;
  }

  /* RCR=1は1周期に1回更新（FOCは頂点、それ以外は底）。旧RCR=0では従来の
   * 半周期割り込みを除外する。電圧モードは開始時にRCR=1を必須にする。 */
  if (motor_timer->Instance->RCR == 0U &&
      __HAL_TIM_GET_COUNTER(motor_timer) > (__HAL_TIM_GET_AUTORELOAD(motor_timer)/2U)) return;

  MotorCalibration_WatchdogISR();
  if (FocVoltage_IsActive()) {
    /* FOC周期処理はADC完了へ移動。ADCが止まってもTIMは独立に停止を監視する。 */
    foc_update_cnt=__HAL_TIM_GET_COUNTER(motor_timer);
    foc_update_down=(motor_timer->Instance->CR1 & TIM_CR1_DIR)!=0U;
    FocVoltage_WatchdogISR();
    return;
  }
  AS5047P_Tick();
  if (MotorControl_IsVoltageMode()) {
    __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_1, voltage_compare[0]);
    __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_2, voltage_compare[1]);
    __HAL_TIM_SET_COMPARE(motor_timer, TIM_CHANNEL_3, voltage_compare[2]);
  }
  if (MotorControl_IsSixStepMode(motor_mode)) MotorControl_SixStepTick();
}

/* 電圧モードでは既存のbootstrap/FAULT確認を再利用する。 */
bool MotorControl_IsStopped(void) { return !outputs_enabled; }
bool MotorControl_IsVoltageMode(void) { return motor_mode == MOTOR_CONTROL_MODE_VOLTAGE; }
HAL_StatusTypeDef MotorControl_StartVoltage(void)
{
  if (!MotorControl_IsStopped()) return HAL_BUSY;
  /* CubeMX再生成前のRCR=0で45usの締切を使うと頂点で混在更新になるため拒否。 */
  if (motor_timer == NULL || motor_timer->Init.RepetitionCounter != 1U ||
      motor_timer->Instance->RCR != 1U ||
      (motor_timer->Instance->CR1 & TIM_CR1_CMS) != TIM_COUNTERMODE_CENTERALIGNED1) {
    printf("Voltage mode blocked: set CubeMX TIM1 Repetition Counter=1, Center Aligned 1\r\n");
    return HAL_ERROR;
  }
  HAL_StatusTypeDef result = MotorControl_StartAtMidpoint();
  if (result != HAL_OK) return result;
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  for (unsigned i=0; i<3; i++) voltage_compare[i] = MotorControl_MidpointCompare();
  motor_mode = MOTOR_CONTROL_MODE_VOLTAGE;
  __set_PRIMASK(mask);
  return HAL_OK;
}
bool MotorControl_SetVoltage(float angle, float vd, float vq, float vm)
{
  float duty[3];
  if (!VoltageVector_Compute(angle, vd, vq, vm, MOTOR_CONTROL_VOLTAGE_LIMIT,
                             MOTOR_CONTROL_PWM_MARGIN, duty)) return false;
  uint32_t compare[3];
  for (unsigned i=0; i<3; i++) compare[i] = (uint32_t)(duty[i] *
      (float)(__HAL_TIM_GET_AUTORELOAD(motor_timer)+1U) + 0.5f);
  /* mainからCCRを直接更新すると更新イベントが3相の途中に来る可能性がある。
   * RAMへ公開する。FOCはADC ISRでpreloadへ書いて次の頂点で反映。
   * 校正は従来どおり底ISRでpreloadへ書いて次の底で反映。 */
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  bool enabled = MotorControl_IsVoltageMode();
  if (enabled) for (unsigned i=0; i<3; i++) voltage_compare[i] = compare[i];
  __set_PRIMASK(mask);
  return enabled;
}

/* ADC完了から呼ぶFOC専用周期処理。CH4立上りは下降時のARR-1付近なので、
 * 2ランクの変換完了は頂点の後となる。CCR反映は次の頂点（1周期後）。 */
void MotorControl_FocAdcISR(void)
{
  if (!FocVoltage_IsActive()) return;
  uint32_t began=foc_adc_began;
  uint32_t arr=__HAL_TIM_GET_AUTORELOAD(motor_timer);
  uint32_t count;
  uint32_t initial=foc_adc_phase;
  FocVoltage_TickISR();
  uint32_t mask=__get_PRIMASK(); __disable_irq();
  count=__HAL_TIM_GET_COUNTER(motor_timer);
  uint32_t phase=(motor_timer->Instance->CR1 & TIM_CR1_DIR) ? arr-count : arr+count;
  /* タイマの頂点から90%以内。途中で次の頂点を越した場合も検出する。
   * TIM IRQはADCと同優先度なので未処理UIFがあり得る。UIFではなく位相で判定。 */
  FocVoltage_CheckDeadlineISR(DWT->CYCCNT-began,
      phase > 2U*arr-arr/5U || phase<initial ||
      (uint32_t)(DWT->CYCCNT-began)>=SystemCoreClock/control_tick_hz);
  if (MotorControl_IsVoltageMode()) {
    __HAL_TIM_SET_COMPARE(motor_timer,TIM_CHANNEL_1,voltage_compare[0]);
    __HAL_TIM_SET_COMPARE(motor_timer,TIM_CHANNEL_2,voltage_compare[1]);
    __HAL_TIM_SET_COMPARE(motor_timer,TIM_CHANNEL_3,voltage_compare[2]);
  }
  __set_PRIMASK(mask);
  /* 出力を停止した後はmain側の低速取得に戻す。処理完了を監視側に通知する。 */
  if (MotorControl_IsVoltageMode()) {
    AS5047P_Tick();
    FocVoltage_AdcCompleteISR();
  }
}

/* ADCコールバック冒頭の位相と時刻を保存し、電流処理を含む締切を測る。 */
void MotorControl_FocAdcBeginISR(void)
{
  if (!FocVoltage_IsActive()) return;
  foc_adc_began=DWT->CYCCNT;
  uint32_t arr=__HAL_TIM_GET_AUTORELOAD(motor_timer);
  foc_adc_cnt=__HAL_TIM_GET_COUNTER(motor_timer);
  foc_adc_down=(motor_timer->Instance->CR1 & TIM_CR1_DIR)!=0U;
  foc_adc_phase=foc_adc_down ? arr-foc_adc_cnt : arr+foc_adc_cnt;
}
void MotorControl_PrintFocPhase(void)
{
  uint32_t mask=__get_PRIMASK(); __disable_irq();
  uint32_t update=foc_update_cnt, adc=foc_adc_cnt;
  bool ud=foc_update_down, ad=foc_adc_down;
  __set_PRIMASK(mask);
  printf("FOC phase: update CNT=%lu DIR=%u, ADC CNT=%lu DIR=%u (ARR=%lu)\r\n",
      (unsigned long)update,ud,(unsigned long)adc,ad,
      (unsigned long)__HAL_TIM_GET_AUTORELOAD(motor_timer));
}
