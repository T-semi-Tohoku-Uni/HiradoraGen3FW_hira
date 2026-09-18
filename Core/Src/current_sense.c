#include "current_sense.h"

#include "console.h"
#include "motor_control.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define CURRENT_SENSE_PWM_OUTPUT_MASK                                  \
  (TIM_CCER_CC1E | TIM_CCER_CC1NE | TIM_CCER_CC2E | TIM_CCER_CC2NE | \
   TIM_CCER_CC3E | TIM_CCER_CC3NE)

#define CURRENT_SENSE_CONSOLE_FLUSH_TIMEOUT_MS 15000U
#define CURRENT_SENSE_ACQUISITION_TIMEOUT_MS 1000U
#define CURRENT_SENSE_OFFSET_SAMPLE_COUNT 1000U
#define CURRENT_SENSE_SLAVE_WAIT_LOOP_LIMIT 1024U
#define CURRENT_SENSE_SECTOR_BITS 3U
#define CURRENT_SENSE_SECTOR_BUFFER_SIZE \
  ((CURRENT_SENSE_SAMPLE_COUNT * CURRENT_SENSE_SECTOR_BITS + 7U) / 8U)

typedef struct
{
  uint8_t byte0;
  uint8_t byte1;
  uint8_t byte2;
  uint8_t byte3;
  uint8_t byte4;
  uint8_t byte5;
} CurrentSenseSample;

_Static_assert(sizeof(CurrentSenseSample) == 6U,
               "CurrentSenseSample must remain packed to six bytes");

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
static CurrentSenseSample samples[CURRENT_SENSE_SAMPLE_COUNT];
static uint8_t sample_sectors[CURRENT_SENSE_SECTOR_BUFFER_SIZE];
static volatile uint32_t captured_sample_count;
static volatile CurrentSenseState current_state = CURRENT_SENSE_UNINITIALIZED;
static uint32_t acquisition_start_tick;
static uint32_t transmit_sample_index;
static bool timer_started_for_capture;
static uint32_t acquisition_sample_count;
static float offsets[4]; /* U1, V, U2, W, in ADC counts. */

_Static_assert(CURRENT_SENSE_OFFSET_SAMPLE_COUNT <= CURRENT_SENSE_SAMPLE_COUNT,
               "Offset samples must fit in the capture buffer");

/* Store four 12-bit values in six bytes so 4000 samples fit in 32 KiB RAM. */
static void CurrentSense_StoreSample(CurrentSenseSample *sample,
                                     uint16_t u1_raw,
                                     uint16_t v_raw,
                                     uint16_t u2_raw,
                                     uint16_t w_raw)
{
  sample->byte0 = (uint8_t)u1_raw;
  sample->byte1 =
    (uint8_t)((u1_raw >> 8U) | (uint16_t)(v_raw << 4U));
  sample->byte2 = (uint8_t)(v_raw >> 4U);
  sample->byte3 = (uint8_t)u2_raw;
  sample->byte4 =
    (uint8_t)((u2_raw >> 8U) | (uint16_t)(w_raw << 4U));
  sample->byte5 = (uint8_t)(w_raw >> 4U);
}

static uint16_t CurrentSense_GetU1Raw(const CurrentSenseSample *sample)
{
  return (uint16_t)((uint16_t)sample->byte0 |
                    ((uint16_t)(sample->byte1 & 0x0FU) << 8U));
}

static uint16_t CurrentSense_GetVRaw(const CurrentSenseSample *sample)
{
  return (uint16_t)(((uint16_t)sample->byte1 >> 4U) |
                    ((uint16_t)sample->byte2 << 4U));
}

static uint16_t CurrentSense_GetWRaw(const CurrentSenseSample *sample)
{
  return (uint16_t)(((uint16_t)sample->byte4 >> 4U) |
                    ((uint16_t)sample->byte5 << 4U));
}

static uint16_t CurrentSense_GetU2Raw(const CurrentSenseSample *sample)
{
  return (uint16_t)((uint16_t)sample->byte3 |
                    ((uint16_t)(sample->byte4 & 0x0FU) << 8U));
}

static void CurrentSense_StoreSector(uint32_t index, uint8_t sector)
{
  const uint32_t bit_index = index * CURRENT_SENSE_SECTOR_BITS;
  const uint32_t byte_index = bit_index / 8U;
  const uint32_t bit_offset = bit_index % 8U;
  const uint16_t packed_sector =
    (uint16_t)((uint16_t)(sector & 0x07U) << bit_offset);

  sample_sectors[byte_index] |= (uint8_t)packed_sector;
  if ((bit_offset > 5U) &&
      ((byte_index + 1U) < CURRENT_SENSE_SECTOR_BUFFER_SIZE))
  {
    sample_sectors[byte_index + 1U] |= (uint8_t)(packed_sector >> 8U);
  }
}

static uint8_t CurrentSense_GetSector(uint32_t index)
{
  const uint32_t bit_index = index * CURRENT_SENSE_SECTOR_BITS;
  const uint32_t byte_index = bit_index / 8U;
  const uint32_t bit_offset = bit_index % 8U;
  uint16_t packed_sector = sample_sectors[byte_index];

  if ((bit_offset > 5U) &&
      ((byte_index + 1U) < CURRENT_SENSE_SECTOR_BUFFER_SIZE))
  {
    packed_sector |= (uint16_t)((uint16_t)sample_sectors[byte_index + 1U]
                                << 8U);
  }

  return (uint8_t)((packed_sector >> bit_offset) & 0x07U);
}

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

  /* Keep prior console messages outside the CSV response. */
  if (Console_Flush(1000U) != HAL_OK)
  {
    printf("ADC capture could not flush the console\r\n");
    return HAL_ERROR;
  }

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
  memset(sample_sectors, 0, sizeof(sample_sectors));
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

static void CurrentSense_BeginCsv(void)
{
  static const char csv_header[] =
    "sample,sector,u1_delta,v_delta,u2_delta,w_delta\r\n";

  transmit_sample_index = 0U;
  (void)Console_Write(csv_header, sizeof(csv_header) - 1U);
}

static bool CurrentSense_SendNextCsvLine(void)
{
  char line[64];

  if (transmit_sample_index < CURRENT_SENSE_SAMPLE_COUNT)
  {
    const uint32_t index = transmit_sample_index;
    const int length = snprintf(line,
                                sizeof(line),
                                "%lu,%u,%.3f,%.3f,%.3f,%.3f\r\n",
                                (unsigned long)index,
                                (unsigned int)CurrentSense_GetSector(index),
                                (double)((float)CurrentSense_GetU1Raw(&samples[index]) - offsets[0]),
                                (double)((float)CurrentSense_GetVRaw(&samples[index]) - offsets[1]),
                                (double)((float)CurrentSense_GetU2Raw(&samples[index]) - offsets[2]),
                                (double)((float)CurrentSense_GetWRaw(&samples[index]) - offsets[3]));

    if ((length > 0) && ((size_t)length < sizeof(line)))
    {
      (void)Console_Write(line, (size_t)length);
    }

    transmit_sample_index++;
    return false;
  }

  (void)Console_Flush(CURRENT_SENSE_CONSOLE_FLUSH_TIMEOUT_MS);
  return true;
}

/* Called only at startup, before MotorControl_Init enables any PWM pins. */
static HAL_StatusTypeDef CurrentSense_CalibrateOffset(void)
{
  uint32_t sums[4] = {0U};
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

  for (uint32_t i = 0U; i < CURRENT_SENSE_OFFSET_SAMPLE_COUNT; i++)
  {
    sums[0] += CurrentSense_GetU1Raw(&samples[i]);
    sums[1] += CurrentSense_GetVRaw(&samples[i]);
    sums[2] += CurrentSense_GetU2Raw(&samples[i]);
    sums[3] += CurrentSense_GetWRaw(&samples[i]);
  }
  for (uint32_t i = 0U; i < 4U; i++)
  {
    offsets[i] = (float)sums[i] / (float)CURRENT_SENSE_OFFSET_SAMPLE_COUNT;
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
  transmit_sample_index = 0U;
  timer_started_for_capture = false;
  current_state = CURRENT_SENSE_IDLE;
  return CurrentSense_CalibrateOffset();
}

bool CurrentSense_ProcessCommand(const char *command)
{
  HAL_StatusTypeDef status;

  if ((command == NULL) || !CurrentSense_IsCommand(command, "adc"))
  {
    return false;
  }

  if (current_state == CURRENT_SENSE_UNINITIALIZED)
  {
    printf("ADC capture is not initialized\r\n");
    return true;
  }
  if (current_state != CURRENT_SENSE_IDLE)
  {
    printf("ADC capture is busy\r\n");
    return true;
  }

  status = CurrentSense_StartAcquisition(CURRENT_SENSE_SAMPLE_COUNT);
  if (status != HAL_OK)
  {
    printf("ADC capture start failed: HAL status=%d\r\n", (int)status);
  }
  else
  {
    printf("ADC capture started: %u samples\r\n",
           (unsigned int)CURRENT_SENSE_SAMPLE_COUNT);
  }

  return true;
}

void CurrentSense_Task(void)
{
  if (current_state == CURRENT_SENSE_SYNC_ERROR)
  {
    const uint32_t adc2_isr = adc_slave->Instance->ISR;
    const uint32_t adc2_ier = adc_slave->Instance->IER;

    current_state = CURRENT_SENSE_TRANSMITTING;
    (void)CurrentSense_StopAcquisition();
    printf("ADC capture synchronization failed: samples=%lu, "
           "ADC2_ISR=0x%08lX, ADC2_IER=0x%08lX\r\n",
           (unsigned long)captured_sample_count,
           (unsigned long)adc2_isr,
           (unsigned long)adc2_ier);
    current_state = CURRENT_SENSE_IDLE;
    return;
  }

  if (current_state == CURRENT_SENSE_ACQUIRING)
  {
    if ((HAL_GetTick() - acquisition_start_tick) <
        CURRENT_SENSE_ACQUISITION_TIMEOUT_MS)
    {
      return;
    }

    const uint32_t timer_count = sample_timer->Instance->CNT;
    const uint32_t adc1_isr = adc_master->Instance->ISR;
    const uint32_t adc1_cr = adc_master->Instance->CR;
    const uint32_t adc1_ier = adc_master->Instance->IER;
    const uint32_t adc2_isr = adc_slave->Instance->ISR;
    const uint32_t adc2_cr = adc_slave->Instance->CR;
    const uint32_t adc2_ier = adc_slave->Instance->IER;

    current_state = CURRENT_SENSE_TRANSMITTING;
    (void)CurrentSense_StopAcquisition();
    printf("ADC capture timed out: samples=%lu, TIM1_CNT=%lu, "
           "ADC1_ISR=0x%08lX, ADC1_CR=0x%08lX, ADC1_IER=0x%08lX, "
           "ADC2_ISR=0x%08lX, ADC2_CR=0x%08lX, ADC2_IER=0x%08lX\r\n",
           (unsigned long)captured_sample_count,
           (unsigned long)timer_count,
           (unsigned long)adc1_isr,
           (unsigned long)adc1_cr,
           (unsigned long)adc1_ier,
           (unsigned long)adc2_isr,
           (unsigned long)adc2_cr,
           (unsigned long)adc2_ier);
    current_state = CURRENT_SENSE_IDLE;
    return;
  }

  if (current_state == CURRENT_SENSE_TRANSMITTING)
  {
    if (CurrentSense_SendNextCsvLine())
    {
      current_state = CURRENT_SENSE_IDLE;
    }
    return;
  }

  if (current_state != CURRENT_SENSE_DATA_READY)
  {
    return;
  }

  current_state = CURRENT_SENSE_TRANSMITTING;

  if (CurrentSense_StopAcquisition() != HAL_OK)
  {
    printf("ADC capture stop failed\r\n");
    current_state = CURRENT_SENSE_IDLE;
    return;
  }

  /* The captured samples are now in RAM; keep the motor stopped while the
   * comparatively slow CSV transfer is in progress. */
  MotorControl_Stop();
  CurrentSense_BeginCsv();
}

bool CurrentSense_IsBusy(void)
{
  return ((current_state == CURRENT_SENSE_ACQUIRING) ||
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
    current_state = CURRENT_SENSE_SYNC_ERROR;
    return;
  }

  index = captured_sample_count;
  if (index >= acquisition_sample_count)
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
  CurrentSense_StoreSample(&samples[index],
                           u1_raw,
                           v_raw,
                           u2_raw,
                           w_raw);
  CurrentSense_StoreSector(index, MotorControl_GetSector());
  __HAL_ADC_CLEAR_FLAG(adc_slave, ADC_FLAG_JEOC | ADC_FLAG_JEOS);

  index++;
  captured_sample_count = index;

  if (index >= acquisition_sample_count)
  {
    /* Main context performs the blocking HAL stop calls and CSV output. */
    __HAL_ADC_DISABLE_IT(adc_master, ADC_IT_JEOC | ADC_IT_JEOS);
    __HAL_ADC_DISABLE_IT(adc_slave, ADC_IT_JEOC | ADC_IT_JEOS);
    current_state = CURRENT_SENSE_DATA_READY;
  }
}
