#include "bus_voltage.h"
#include "motor_control_config.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if MOTOR_CONTROL_VM_SAMPLE_PERIOD_MS == 0U || MOTOR_CONTROL_VM_TIMEOUT_MS == 0U || MOTOR_CONTROL_VM_STALE_MS == 0U
#error "VM timing parameters must be nonzero"
#endif

static ADC_HandleTypeDef *bus_adc;
static BusVoltageSample latest;
static bool active;
static bool disabled;
static bool timely;
static uint8_t rank_index;
static uint16_t values[2];
static uint32_t started_ms;
static uint32_t next_ms;
static uint32_t errors;
static uint32_t generation;

/* regularだけを停止。HAL_ADC_Stopはinjectedにも作用するため使用しない。 */
static void Finish(bool success)
{
  if (HAL_ADCEx_RegularStop(bus_adc) != HAL_OK) {
    disabled = true; /* ランク位置が不明な状態では再開しない。 */
    success = false;
  }
  active = false;
  next_ms = HAL_GetTick();
  if (!success) {
    latest.valid = false;
    errors++;
  }
}

void BusVoltage_Task(void)
{
  if (bus_adc == NULL || disabled) return;
  uint32_t now = HAL_GetTick();
  if (!active) {
    if ((uint32_t)(now - next_ms) < MOTOR_CONTROL_VM_SAMPLE_PERIOD_MS) return;
    rank_index = 0U;
    timely = true;
    active = true;
    started_ms = now;
    if (HAL_ADC_Start(bus_adc) != HAL_OK) Finish(false);
    return;
  }
  if (__HAL_ADC_GET_FLAG(bus_adc, ADC_FLAG_OVR) != 0U ||
      (uint32_t)(now - started_ms) >= MOTOR_CONTROL_VM_TIMEOUT_MS) {
    Finish(false);
    return;
  }
  /* EOCSelectionはJEOS割り込みのためSEQのまま。regularはEOCを直接確認。 */
  if (__HAL_ADC_GET_FLAG(bus_adc, ADC_FLAG_EOC) == 0U) return;
  uint16_t raw = (uint16_t)HAL_ADC_GetValue(bus_adc);
  bool end = __HAL_ADC_GET_FLAG(bus_adc, ADC_FLAG_EOS) != 0U;
  if (end != ((rank_index & 1U) != 0U)) {
    Finish(false); /* VREFINTとVMの取り違えを防止。 */
    return;
  }
  /* 休止後の最初の変換を捨てるため、最初のVREFINT/VM一組を捨てる。
   * バースト中に1 tickでも跨いだ場合は保守的に不採用（ES0431対策）。 */
  if ((uint32_t)(now - started_ms) != 0U) timely = false;
  if (rank_index >= 2U) values[rank_index - 2U] = raw;
  if (++rank_index < 4U) {
    if (HAL_ADC_Start(bus_adc) != HAL_OK) Finish(false);
    return;
  }
  if (!timely || values[0] == 0U || values[0] >= 4095U || values[1] >= 4095U) {
    Finish(false);
    return;
  }
  float vref = ((float)VREFINT_CAL_VREF / 1000.0f) *
               (float)(*VREFINT_CAL_ADDR) / (float)values[0];
  if (vref < 1.62f || vref > 3.6f) {
    Finish(false);
    return;
  }
  Finish(true);
  if (disabled) return;
  latest.raw = values[1];
  latest.vref_volts = vref;
  /* 100k/10k分圧をVMへ戻す。0Vは正常な測定値として扱う。 */
  latest.volts = (float)values[1] * vref / 4095.0f *
    (1.0f + MOTOR_CONTROL_VM_DIVIDER_TOP_OHMS / MOTOR_CONTROL_VM_DIVIDER_BOTTOM_OHMS);
  latest.updated_ms = HAL_GetTick();
  latest.valid = true;
  latest.overvoltage = latest.volts >= MOTOR_CONTROL_VM_MAX_VOLTS;
  generation++;
}

HAL_StatusTypeDef BusVoltage_Init(ADC_HandleTypeDef *adc)
{
  if (adc == NULL || adc->Instance != ADC1 ||
      adc->Init.NbrOfConversion != 2U || adc->Init.DiscontinuousConvMode != ENABLE ||
      adc->Init.NbrOfDiscConversion != 1U || adc->Init.EOCSelection != ADC_EOC_SEQ_CONV ||
      *VREFINT_CAL_ADDR == 0U || *VREFINT_CAL_ADDR >= 4095U ||
      MOTOR_CONTROL_VM_DIVIDER_TOP_OHMS <= 0.0f ||
      MOTOR_CONTROL_VM_DIVIDER_BOTTOM_OHMS <= 0.0f) return HAL_ERROR;
  bus_adc = adc;
  memset(&latest, 0, sizeof(latest));
  active = disabled = false;
  errors = generation = 0U;
  /* VREFINTの起動整定。PWM OFFの起動処理だけは待機を許可する。 */
  HAL_Delay(1U);
  next_ms = HAL_GetTick() - MOTOR_CONTROL_VM_SAMPLE_PERIOD_MS;
  float vref_sum = 0.0f, vm_sum = 0.0f;
  uint32_t count = 0U, seen = 0U;
  uint32_t start = HAL_GetTick();
  while (count < 64U && !disabled && (uint32_t)(HAL_GetTick() - start) < 1000U) {
    BusVoltage_Task();
    if (generation != seen) {
      seen = generation;
      vref_sum += latest.vref_volts;
      vm_sum += latest.volts;
      count++;
    }
  }
  if (active) Finish(false);
  if (count != 64U || disabled) return HAL_ERROR;
  latest.vref_volts = vref_sum / 64.0f;
  latest.volts = vm_sum / 64.0f;
  latest.overvoltage = latest.volts >= MOTOR_CONTROL_VM_MAX_VOLTS;
  return HAL_OK;
}

bool BusVoltage_IsAcquiring(void) { return active; }

bool BusVoltage_GetSample(BusVoltageSample *sample)
{
  if (sample == NULL) return false;
  *sample = latest;
  sample->valid = latest.valid &&
    (uint32_t)(HAL_GetTick() - latest.updated_ms) < MOTOR_CONTROL_VM_STALE_MS;
  return sample->valid;
}

void BusVoltage_Print(void)
{
  BusVoltageSample sample;
  bool valid = BusVoltage_GetSample(&sample);
  printf("VM=%.3f V, VREF+=%.4f V, raw=%u, %s, age=%lu ms, "
         "nominal=%.1f V, limit=%.1f V, errors=%lu\r\n",
         (double)sample.volts, (double)sample.vref_volts, (unsigned int)sample.raw,
         !valid ? "INVALID/STALE" : sample.overvoltage ? "OVER LIMIT" : "OK",
         (unsigned long)(HAL_GetTick() - sample.updated_ms),
         (double)MOTOR_CONTROL_VM_NOMINAL_VOLTS, (double)MOTOR_CONTROL_VM_MAX_VOLTS,
         (unsigned long)errors);
}

bool BusVoltage_ProcessCommand(const char *command)
{
  if (command == NULL) return false;
  while (isspace((unsigned char)*command)) command++;
  if (tolower((unsigned char)command[0]) != 'v') return false;
  if (tolower((unsigned char)command[1]) != 'm') return false;
  command += 2;
  while (isspace((unsigned char)*command)) command++;
  if (*command != '\0') return false;
  BusVoltage_Print();
  return true;
}
