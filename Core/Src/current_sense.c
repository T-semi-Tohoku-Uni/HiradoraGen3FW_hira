#include "current_sense.h"

#include "dma_logger.h"
#include "bus_voltage.h"
#include "motor_control.h"
#include "motor_calibration.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#define CURRENT_SENSE_PWM_OUTPUT_MASK                                  \
  (TIM_CCER_CC1E | TIM_CCER_CC1NE | TIM_CCER_CC2E | TIM_CCER_CC2NE | \
   TIM_CCER_CC3E | TIM_CCER_CC3NE)

#define CURRENT_SENSE_ACQUISITION_TIMEOUT_MS 1000U
#define CURRENT_SENSE_OFFSET_SAMPLE_COUNT 1000U
#define CURRENT_SENSE_SHUNT_OHMS 0.001f
#define CURRENT_SENSE_PGA_GAIN 8.0f
/* 1.5k series, with 22k to 3.3V and 22k to GND: 11k / 12.5k. */
#define CURRENT_SENSE_INPUT_ATTENUATION (11.0f / 12.5f)
#define CURRENT_SENSE_ADC_FULL_SCALE 4095U
#define CURRENT_SENSE_SLAVE_WAIT_LOOP_LIMIT 1024U
typedef enum
{
  CURRENT_SENSE_UNINITIALIZED = 0,
  CURRENT_SENSE_IDLE,
  CURRENT_SENSE_ACQUIRING,
  CURRENT_SENSE_DATA_READY,
  CURRENT_SENSE_SYNC_ERROR,
  CURRENT_SENSE_TRANSMITTING
} CurrentSenseState;

static ADC_HandleTypeDef *adc_master;
static ADC_HandleTypeDef *adc_slave;
static TIM_HandleTypeDef *sample_timer;
static volatile uint32_t offset_sums[4];
static volatile uint32_t last_sample_tick;
static volatile uint32_t captured_sample_count;
static volatile CurrentSenseState current_state = CURRENT_SENSE_UNINITIALIZED;
static uint32_t acquisition_start_tick;
static bool timer_started_for_capture;
static uint32_t acquisition_sample_count;
static float offsets[4]; /* U1, V, U2, W, in ADC counts. */
static float amps_per_count;
static volatile bool control_acquisition, log_requested;

static void CurrentSense_DisableTrigger(void)
{
  /* Disable only CH4 so running motor PWM channels are unaffected. */
  CLEAR_BIT(sample_timer->Instance->CCER, TIM_CCER_CC4E);

  if (timer_started_for_capture)
  {
    CLEAR_BIT(sample_timer->Instance->CR1, TIM_CR1_CEN);
    CLEAR_BIT(sample_timer->Instance->BDTR, TIM_BDTR_MOE);
    __HAL_TIM_SET_COUNTER(sample_timer, 0U);
    timer_started_for_capture = false;
  }
}

static const char *CurrentSense_SkipSpaces(const char *text)
{
  while ((*text != '\0') && (isspace((unsigned char)*text) != 0))
  {
    text++;
  }
  return text;
}

static bool CurrentSense_IsCommand(const char *text, const char *expected)
{
  text = CurrentSense_SkipSpaces(text);

  while ((*text != '\0') && (*expected != '\0'))
  {
    if (tolower((unsigned char)*text) != tolower((unsigned char)*expected))
    {
      return false;
    }
    text++;
    expected++;
  }

  text = CurrentSense_SkipSpaces(text);
  return ((*text == '\0') && (*expected == '\0'));
}

static HAL_StatusTypeDef CurrentSense_StartAcquisition(uint32_t sample_count)
{
  HAL_StatusTypeDef status;

  timer_started_for_capture =
    (READ_BIT(sample_timer->Instance->CR1, TIM_CR1_CEN) == 0U);

  /*
   * The ADC trigger uses the internal CH4 output edge, not merely CC4IF.
   * Expose CH4 before arming the ADC so enabling it cannot become sample 0.
   */
  __HAL_TIM_CLEAR_FLAG(sample_timer, TIM_FLAG_CC4);
  SET_BIT(sample_timer->Instance->CCER, TIM_CCER_CC4E);

  if (timer_started_for_capture)
  {
    /* Generate CH4 internally while keeping all motor pins disabled. */
    CLEAR_BIT(sample_timer->Instance->BDTR, TIM_BDTR_MOE);
    CLEAR_BIT(sample_timer->Instance->CCER,
              CURRENT_SENSE_PWM_OUTPUT_MASK);
    __HAL_TIM_SET_COUNTER(sample_timer, 0U);
    sample_timer->Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(sample_timer, TIM_FLAG_UPDATE | TIM_FLAG_CC4);
    SET_BIT(sample_timer->Instance->BDTR, TIM_BDTR_MOE);
    SET_BIT(sample_timer->Instance->CR1, TIM_CR1_CEN);
  }

  captured_sample_count = 0U;
  acquisition_sample_count = sample_count;
  last_sample_tick = HAL_GetTick();
  current_state = CURRENT_SENSE_ACQUIRING;

  /*
   * In dual injected mode, the slave must be armed before the master.
   * Do not use the slave HAL interrupt: because its JSQR has no independent
   * external trigger, HAL disables JEOSIE after the first slave sequence.
   */
  __HAL_ADC_DISABLE_IT(adc_slave, ADC_IT_JEOC | ADC_IT_JEOS);
  status = HAL_ADCEx_InjectedStart(adc_slave);
  if (status != HAL_OK)
  {
    CurrentSense_DisableTrigger();
    current_state = CURRENT_SENSE_IDLE;
    return status;
  }

  status = HAL_ADCEx_InjectedStart_IT(adc_master);
  if (status != HAL_OK)
  {
    (void)HAL_ADCEx_InjectedStop(adc_slave);
    CurrentSense_DisableTrigger();
    current_state = CURRENT_SENSE_IDLE;
    return status;
  }

  acquisition_start_tick = HAL_GetTick();

  return HAL_OK;
}

static HAL_StatusTypeDef CurrentSense_StopAcquisition(void)
{
  HAL_StatusTypeDef result = HAL_OK;

  CurrentSense_DisableTrigger();

  /* Stop the master before the slave as required for dual injected mode. */
  if (HAL_ADCEx_InjectedStop_IT(adc_master) != HAL_OK)
  {
    result = HAL_ERROR;
  }
  if (HAL_ADCEx_InjectedStop(adc_slave) != HAL_OK)
  {
    result = HAL_ERROR;
  }

  return result;
}

/* ADC1 regularの2ランクをVMモジュールで取得し、実測VREF+で電流を換算。
 * 起動時PWM OFFで実行し、injected電流ゼロ点校正は従来のまま維持する。 */
static HAL_StatusTypeDef CurrentSense_CalibrateScale(void)
{
  BusVoltageSample sample;
  HAL_StatusTypeDef status = BusVoltage_Init(adc_master);
  if (status != HAL_OK || !BusVoltage_GetSample(&sample)) {
    printf("ADC VREFINT/VM initialization failed\r\n");
    return HAL_ERROR;
  }
  amps_per_count = sample.vref_volts /
    ((float)CURRENT_SENSE_ADC_FULL_SCALE * CURRENT_SENSE_PGA_GAIN *
     CURRENT_SENSE_INPUT_ATTENUATION * CURRENT_SENSE_SHUNT_OHMS);
  printf("ADC scale calibrated: 64 pairs, VREF+=%.4f V, A/count=%.6f\r\n",
         (double)sample.vref_volts, (double)amps_per_count);
  BusVoltage_Print();
  return HAL_OK;
}

/* Called only at startup, before MotorControl_Init enables any PWM pins. */
static HAL_StatusTypeDef CurrentSense_CalibrateOffset(void)
{
  for (unsigned int i = 0U; i < 4U; i++) offset_sums[i] = 0U;
  HAL_StatusTypeDef status;

  HAL_Delay(5U); /* Allow the analog path to settle after OPAMP startup. */
  status = CurrentSense_StartAcquisition(CURRENT_SENSE_OFFSET_SAMPLE_COUNT);
  if (status != HAL_OK)
  {
    current_state = CURRENT_SENSE_UNINITIALIZED;
    return status;
  }

  while (current_state == CURRENT_SENSE_ACQUIRING)
  {
    if ((HAL_GetTick() - acquisition_start_tick) >=
        CURRENT_SENSE_ACQUISITION_TIMEOUT_MS)
    {
      status = HAL_TIMEOUT;
      break;
    }
  }
  if ((status == HAL_OK) && (current_state != CURRENT_SENSE_DATA_READY))
  {
    status = HAL_ERROR;
  }
  /* Prevent callbacks from writing the buffer during cleanup. */
  current_state = CURRENT_SENSE_UNINITIALIZED;
  if (CurrentSense_StopAcquisition() != HAL_OK)
  {
    status = HAL_ERROR;
  }
  if (status != HAL_OK)
  {
    printf("ADC offset calibration failed: HAL status=%d, samples=%lu\r\n",
           (int)status, (unsigned long)captured_sample_count);
    return status;
  }

  for (uint32_t i = 0U; i < 4U; i++)
  {
    offsets[i] = (float)offset_sums[i] / (float)CURRENT_SENSE_OFFSET_SAMPLE_COUNT;
  }
  printf("ADC offset calibrated: %u samples, U1=%.3f, V=%.3f, U2=%.3f, W=%.3f\r\n",
         (unsigned int)CURRENT_SENSE_OFFSET_SAMPLE_COUNT,
         (double)offsets[0], (double)offsets[1],
         (double)offsets[2], (double)offsets[3]);
  current_state = CURRENT_SENSE_IDLE;
  return HAL_OK;
}

HAL_StatusTypeDef CurrentSense_Init(ADC_HandleTypeDef *master_adc,
                                    ADC_HandleTypeDef *slave_adc,
                                    OPAMP_HandleTypeDef *u_opamp,
                                    OPAMP_HandleTypeDef *v_opamp,
                                    OPAMP_HandleTypeDef *w_opamp,
                                    TIM_HandleTypeDef *trigger_timer)
{
  if ((master_adc == NULL) || (slave_adc == NULL) ||
      (u_opamp == NULL) || (v_opamp == NULL) || (w_opamp == NULL) ||
      (trigger_timer == NULL) ||
      (master_adc->Instance != ADC1) || (slave_adc->Instance != ADC2) ||
      (trigger_timer->Instance != TIM1))
  {
    return HAL_ERROR;
  }

  adc_master = master_adc;
  adc_slave = slave_adc;
  sample_timer = trigger_timer;

  if ((READ_BIT(sample_timer->Instance->CR1, TIM_CR1_CEN) != 0U) ||
      (READ_BIT(sample_timer->Instance->CCER, CURRENT_SENSE_PWM_OUTPUT_MASK) != 0U))
  {
    return HAL_ERROR; /* Zero-current calibration requires disabled motor PWM. */
  }

  if (HAL_ADCEx_Calibration_Start(adc_master, ADC_SINGLE_ENDED) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_ADCEx_Calibration_Start(adc_slave, ADC_SINGLE_ENDED) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (CurrentSense_CalibrateScale() != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_OPAMP_Start(u_opamp) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_OPAMP_Start(v_opamp) != HAL_OK)
  {
    (void)HAL_OPAMP_Stop(u_opamp);
    return HAL_ERROR;
  }
  if (HAL_OPAMP_Start(w_opamp) != HAL_OK)
  {
    (void)HAL_OPAMP_Stop(v_opamp);
    (void)HAL_OPAMP_Stop(u_opamp);
    return HAL_ERROR;
  }

  captured_sample_count = 0U;
  timer_started_for_capture = false;
  current_state = CURRENT_SENSE_IDLE;
  return CurrentSense_CalibrateOffset();
}

static void CurrentSense_EndStream(void)
{
  log_requested = false;
  if (control_acquisition && current_state == CURRENT_SENSE_ACQUIRING) {
    DmaLogger_Stop(); /* 電流保護用ADCは停止しない。 */
    printf("ADC logging disabled; current monitoring continues (queued data may follow)\r\n");
    return;
  }
  /* Stop ISR writes before sealing a partially filled block. */
  current_state = CURRENT_SENSE_TRANSMITTING;
  if (CurrentSense_StopAcquisition() != HAL_OK) {
    printf("ADC stop failed; reinitialize before restarting\r\n");
    current_state = CURRENT_SENSE_UNINITIALIZED;
  }
  DmaLogger_Stop();
}

bool CurrentSense_ProcessCommand(const char *command)
{
  if (command == NULL) return false;
  command = CurrentSense_SkipSpaces(command);
  if (tolower((unsigned char)command[0]) != 'a' ||
      tolower((unsigned char)command[1]) != 'd' ||
      tolower((unsigned char)command[2]) != 'c' ||
      (command[3] != '\0' && !isspace((unsigned char)command[3]))) return false;
  const char *argument = CurrentSense_SkipSpaces(command + 3);
  if (CurrentSense_IsCommand(argument, "stop")) {
    if (current_state == CURRENT_SENSE_ACQUIRING ||
        current_state == CURRENT_SENSE_SYNC_ERROR) CurrentSense_EndStream();
    return true;
  }
  if (CurrentSense_IsCommand(argument, "status")) {
    const char *state = control_acquisition && !log_requested ?
                        (DmaLogger_IsBusy() ? "draining (control ADC active)" : "off (control ADC active)") :
                        current_state == CURRENT_SENSE_ACQUIRING ? "streaming" :
                        current_state == CURRENT_SENSE_TRANSMITTING ? "draining" :
                        current_state == CURRENT_SENSE_IDLE ? "idle" :
                        current_state == CURRENT_SENSE_UNINITIALIZED ? "uninitialized" : "error";
    printf("ADC logger: %s, overrun=%lu\r\n",
           state, (unsigned long)DmaLogger_Overruns());
    if (control_acquisition) {
      /* ADC取得とログ表示は別。表示停止中も取得番号が増えることを確認できる。 */
      const uint32_t tick = last_sample_tick;
      printf("ADC control: samples=%lu, age=%lu ms\r\n",
             (unsigned long)captured_sample_count, (unsigned long)(HAL_GetTick()-tick));
    }
    return true;
  }
  uint32_t decimation = DMA_LOGGER_DEFAULT_DECIMATION;
  if (*argument != '\0') {
    char *end;
    const unsigned long value = strtoul(argument, &end, 10);
    if (!isdigit((unsigned char)*argument) || value < 1UL || value > 1000UL ||
        *CurrentSense_SkipSpaces(end) != '\0') {
      printf("Usage: adc [1..1000], adc stop, adc status\r\n");
      return true;
    }
    decimation = (uint32_t)value;
  }
  if (MotorCalibration_IsActive() && !MotorControl_IsVoltageMode()) {
    printf("Calibration is checking standstill; start adc after the voltage ramp begins\r\n");
    return true;
  }
  if (current_state == CURRENT_SENSE_UNINITIALIZED) {
    printf("ADC logger is not initialized\r\n");
    return true;
  }
  if ((!control_acquisition && current_state != CURRENT_SENSE_IDLE) || DmaLogger_IsBusy()) {
    printf("ADC logger busy; send 'adc stop' first\r\n");
    return true;
  }
  /* TIM1 center-aligned counter: one rising CH4 trigger per full cycle. */
  uint32_t timer_clock = HAL_RCC_GetPCLK2Freq();
  if ((RCC->CFGR & RCC_CFGR_PPRE2) != 0U) timer_clock *= 2U;
  const uint32_t period_ns = (uint32_t)(
    (2000000000ULL * sample_timer->Instance->ARR *
     (sample_timer->Instance->PSC + 1U)) / timer_clock);
  DmaLogger_Start(decimation, period_ns, offsets, amps_per_count);
  /* Zero sample limit selects continuous acquisition, not calibration. */
  log_requested = true;
  const HAL_StatusTypeDef status = control_acquisition ? HAL_OK : CurrentSense_StartAcquisition(0U);
  if (status != HAL_OK) {
    log_requested = false; DmaLogger_Stop();
    printf("ADC logger start failed: HAL status=%d\r\n", (int)status);
  } else {
    printf("ADC DMA logger started: decimation=%lu, period_ns=%lu\r\n",
           (unsigned long)decimation, (unsigned long)period_ns);
  }
  return true;
}

void CurrentSense_Task(void)
{
  /* Read the ISR timestamp before the clock: an IRQ between these reads
   * must not make an unsigned subtraction interpret a future tick as timeout. */
  const uint32_t sample_tick = last_sample_tick;
  if (current_state == CURRENT_SENSE_SYNC_ERROR ||
      (current_state == CURRENT_SENSE_ACQUIRING &&
       (uint32_t)(HAL_GetTick() - sample_tick) >=
         CURRENT_SENSE_ACQUISITION_TIMEOUT_MS)) {
    MotorCalibration_TripISR("ADC acquisition error");
    printf("ADC logger stopped: synchronization error or trigger timeout\r\n");
    CurrentSense_EndStream();
  }
  DmaLogger_Task();
  if (current_state == CURRENT_SENSE_TRANSMITTING && !DmaLogger_IsBusy()) {
    current_state = CURRENT_SENSE_IDLE;
    printf("ADC DMA logger stopped: overrun=%lu\r\n",
           (unsigned long)DmaLogger_Overruns());
  }
}

bool CurrentSense_IsBusy(void)
{
  return (DmaLogger_IsBusy() ||
          (current_state == CURRENT_SENSE_ACQUIRING) ||
          (current_state == CURRENT_SENSE_DATA_READY) ||
          (current_state == CURRENT_SENSE_SYNC_ERROR) ||
          (current_state == CURRENT_SENSE_TRANSMITTING));
}

void HAL_ADCEx_InjectedConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  uint32_t index;
  uint16_t u1_raw;
  uint16_t v_raw;
  uint16_t u2_raw;
  uint16_t w_raw;
  uint32_t wait_count;

  if ((hadc != adc_master) || (current_state != CURRENT_SENSE_ACQUIRING))
  {
    return;
  }

  /*
   * The ADC1 JEOS interrupt indicates that its two-rank sequence is complete.
   * Also confirm ADC2 JEOS before reading all four result registers. This
   * avoids relying on the one-shot slave HAL interrupt.
   */
  wait_count = CURRENT_SENSE_SLAVE_WAIT_LOOP_LIMIT;
  while ((__HAL_ADC_GET_FLAG(adc_slave, ADC_FLAG_JEOS) == 0U) &&
         (wait_count > 0U))
  {
    wait_count--;
  }

  if (wait_count == 0U)
  {
    __HAL_ADC_DISABLE_IT(adc_master, ADC_IT_JEOC | ADC_IT_JEOS);
    MotorCalibration_TripISR("ADC synchronization error");
    current_state = CURRENT_SENSE_SYNC_ERROR;
    return;
  }

  index = captured_sample_count;
  if ((acquisition_sample_count != 0U) && (index >= acquisition_sample_count))
  {
    return;
  }

  u1_raw =
    (uint16_t)HAL_ADCEx_InjectedGetValue(adc_master, ADC_INJECTED_RANK_1);
  v_raw =
    (uint16_t)HAL_ADCEx_InjectedGetValue(adc_slave, ADC_INJECTED_RANK_1);
  u2_raw =
    (uint16_t)HAL_ADCEx_InjectedGetValue(adc_master, ADC_INJECTED_RANK_2);
  w_raw =
    (uint16_t)HAL_ADCEx_InjectedGetValue(adc_slave, ADC_INJECTED_RANK_2);
  if (acquisition_sample_count != 0U) {
    offset_sums[0] += u1_raw;
    offset_sums[1] += v_raw;
    offset_sums[2] += u2_raw;
    offset_sums[3] += w_raw;
  } else if (log_requested) {
    DmaLogger_Push(index, u1_raw, v_raw, u2_raw, w_raw, MotorControl_GetSector());
  }
  if (control_acquisition) {
    const uint16_t raw[4] = {u1_raw, v_raw, u2_raw, w_raw};
    float currents[4];
    bool rails = false;
    for (unsigned i=0; i<4; i++) {
      currents[i] = ((float)raw[i] - offsets[i]) * amps_per_count;
      if (raw[i] < 16U || raw[i] > 4079U) rails = true;
    }
    MotorCalibration_CurrentISR(currents, rails);
  }
  last_sample_tick = HAL_GetTick();
  __HAL_ADC_CLEAR_FLAG(adc_slave, ADC_FLAG_JEOC | ADC_FLAG_JEOS);

  index++;
  captured_sample_count = index;

  if ((acquisition_sample_count != 0U) && (index >= acquisition_sample_count))
  {
    /* Main context performs the HAL stop calls. */
    __HAL_ADC_DISABLE_IT(adc_master, ADC_IT_JEOC | ADC_IT_JEOS);
    __HAL_ADC_DISABLE_IT(adc_slave, ADC_IT_JEOC | ADC_IT_JEOS);
    current_state = CURRENT_SENSE_DATA_READY;
  }
}

HAL_StatusTypeDef CurrentSense_BeginControl(void)
{
  if (CurrentSense_IsBusy() || current_state != CURRENT_SENSE_IDLE) return HAL_BUSY;
  control_acquisition = true; log_requested = false;
  HAL_StatusTypeDef result = CurrentSense_StartAcquisition(0U);
  /* MotorControlがPWMを所有するので、ADC側ではTIM1を停止しない。 */
  if (result == HAL_OK) timer_started_for_capture = false;
  else control_acquisition = false;
  return result;
}
void CurrentSense_EndControl(void)
{
  if (!control_acquisition) return;
  control_acquisition = false;
  CurrentSense_EndStream();
}
