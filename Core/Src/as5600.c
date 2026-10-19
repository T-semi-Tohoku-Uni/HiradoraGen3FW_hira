/* AS5047P acquisition/publication/console lifecycle adapted for AS5600.
 * TIM1/ADC initiates rate-limited reads; recovery/configuration stays in main. */
#include "as5600.h"
#include "main.h"
#include "motor_control.h"
#include "motor_control_config.h"
#include <ctype.h>
#include <stdio.h>

#define DEVICE_ADDRESS (0x36U << 1U)
#define REG_ANGLE 0x0CU
#define REG_DIAG 0x0BU
#define REG_CONF 0x07U
#define TWO_PI 6.2831853071795864769f
#define SAMPLE_STALE_MS MOTOR_CONTROL_ENCODER_STALE_MS
#define DIAG_PERIOD_MS MOTOR_CONTROL_ENCODER_DIAG_PERIOD_MS
#define DIAG_STALE_MS MOTOR_CONTROL_ENCODER_DIAG_STALE_MS
#define RECOVERY_DELAY_MS 100U
typedef enum { OFF, IDLE, REQUEST, FAILED, RECOVERING, DECODING } TransferState;
static I2C_HandleTypeDef *encoder_i2c;
static TIM_HandleTypeDef *sample_timer;
static volatile TransferState state;
/* DMA storage must outlive the transaction. */
static uint8_t rx_bytes[2];
static uint16_t address;
static volatile AS5600_Sample samples[2];
static volatile uint8_t published;
#define latest samples[published]
static volatile bool diagnostic_ok;
static volatile uint8_t diagnostic;
static volatile uint32_t diagnostic_ms, start_cycles;
static volatile uint32_t transfers, i2c_errors, sensor_errors, timeouts;
static volatile uint32_t transfer_cycles, max_transfer_cycles;
static volatile uint32_t last_start_cycles;
static volatile bool launched;
static volatile bool configured;
static volatile bool timer_request, previous_timer_angle;
static volatile uint32_t last_timer_reference_cycles, tim_interval_max_cycles, tim_age_max_cycles;
static uint16_t configuration;
static uint32_t fallback_ms, report_ms, recovery_ms;
static bool streaming;
static unsigned status_line;
static const char *volatile first_fault;
static volatile uint32_t fault_error, fault_isr, fault_cycles;

static void RecordFault(const char *reason)
{
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  if (!first_fault) {
    fault_error = HAL_I2C_GetError(encoder_i2c);
    fault_isr = encoder_i2c->Instance->ISR;
    fault_cycles = DWT->CYCCNT - start_cycles;
    first_fault = reason;
  }
  __set_PRIMASK(mask);
}
static bool Expired(void)
{
  return (uint32_t)(DWT->CYCCNT - start_cycles) >
    (SystemCoreClock / 1000000U) * MOTOR_CONTROL_ENCODER_TIMEOUT_US;
}
static void Fail(void)
{
  latest.valid = false; diagnostic_ok = false; state = FAILED;
}
static void StartRead(void)
{
  /* Main and priority-0 TIM/ADC may meet here. Serialize the state and bounded
   * HAL launch, never the wire transfer; callbacks cannot publish mid-launch. */
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  uint32_t now = DWT->CYCCNT;
  if (state != IDLE || !configured || (launched &&
      (uint32_t)(now-last_start_cycles) < (SystemCoreClock/1000000U)*MOTOR_CONTROL_ENCODER_SAMPLE_PERIOD_US)) {
    __set_PRIMASK(mask); return;
  }
  address = !diagnostic_ok ||
    (uint32_t)(HAL_GetTick() - diagnostic_ms) >= DIAG_PERIOD_MS ? REG_DIAG : REG_ANGLE;
  timer_request = sample_timer && (sample_timer->Instance->CR1 & TIM_CR1_CEN);
  state = REQUEST;
  start_cycles = now; last_start_cycles = now; launched = true;
  HAL_StatusTypeDef result = HAL_I2C_Mem_Read_DMA(encoder_i2c, DEVICE_ADDRESS,
      address, I2C_MEMADD_SIZE_8BIT, rx_bytes, address == REG_ANGLE ? 2U : 1U);
  if (result != HAL_OK) {
    RecordFault(result == HAL_BUSY ? "I2C busy" : "I2C launch error");
    i2c_errors++; Fail();
  }
  __set_PRIMASK(mask);
}
void AS5600_Tick(void)
{
  if (encoder_i2c == NULL || state == OFF || state == RECOVERING) return;
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  if (state == REQUEST && Expired()) {
    RecordFault("periodic transfer timeout"); timeouts++; Fail();
  }
  if (sample_timer && (sample_timer->Instance->CR1 & TIM_CR1_CEN)) {
    /* Exclude the transition from slow main sampling to the first TIM sample.
     * Age is read-reference age, not the sensor's internal propagation delay. */
    if (previous_timer_angle && latest.valid && diagnostic_ok) {
      uint32_t age = DWT->CYCCNT-latest.reference_cycles;
      if (age > tim_age_max_cycles) tim_age_max_cycles = age;
    }
  } else previous_timer_angle = false;
  __set_PRIMASK(mask);
  StartRead();
}
static void FrameComplete(void)
{
  if (state != REQUEST) return; /* Ignore completion after timeout or Pause. */
  if (Expired()) {
    RecordFault("completion timeout"); timeouts++; Fail(); return;
  }
  state = DECODING;
  uint32_t received = DWT->CYCCNT;
  transfer_cycles = received - start_cycles;
  if (transfer_cycles > max_transfer_cycles) max_transfer_cycles = transfer_cycles;
  transfers++;
  if (address == REG_DIAG) {
    diagnostic = rx_bytes[0]; diagnostic_ms = HAL_GetTick();
    /* MD=1, ML=0, MH=0. ACK alone does not establish magnet validity. */
    diagnostic_ok = (diagnostic & 0x38U) == 0x20U;
    if (!diagnostic_ok) {
      RecordFault("magnet diagnostic"); sensor_errors++; latest.valid = false;
    }
  } else {
    uint16_t raw = ((uint16_t)(rx_bytes[0] & 0x0FU) << 8U) | rx_bytes[1];
    uint8_t next = published ^ 1U;
    volatile AS5600_Sample *sample = &samples[next];
    sample->raw = raw;
    sample->mechanical_rad = (float)raw * (TWO_PI / 4096.0f);
    sample->electrical_rad = (float)(((uint32_t)raw*MOTOR_CONTROL_POLE_PAIRS) & 0x0FFFU)*(TWO_PI/4096.0f);
    /* These are communication timestamps, not the internal measurement time. */
    sample->request_cycles = start_cycles; sample->received_cycles = received;
    sample->reference_cycles = start_cycles;
    sample->updated_ms = HAL_GetTick(); sample->sequence = latest.sequence + 1U;
    sample->valid = diagnostic_ok;
    /* Publication and readiness must become visible together to TIM/ADC.
     * Otherwise an ISR could start a new transfer before this callback exits. */
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    if (timer_request && previous_timer_angle) {
      uint32_t interval = start_cycles-last_timer_reference_cycles;
      if (interval > tim_interval_max_cycles) tim_interval_max_cycles = interval;
    }
    previous_timer_angle = timer_request;
    last_timer_reference_cycles = start_cycles;
    __DMB();
    published = next;
    state = IDLE;
    __set_PRIMASK(mask);
    return;
  }
  state = IDLE;
}
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *i2c)
{
  if (i2c == encoder_i2c) FrameComplete();
}
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *i2c)
{
  if (i2c != encoder_i2c || state != REQUEST) return;
  RecordFault("I2C transfer error"); i2c_errors++; Fail();
}
bool AS5600_GetSample(AS5600_Sample *sample)
{
  if (sample == NULL) return false;
  /* Keep angle, timestamps and sequence from one published transaction. */
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  *sample = latest;
  bool good = diagnostic_ok;
  uint32_t diag_time = diagnostic_ms;
  __set_PRIMASK(mask);
  uint32_t now = HAL_GetTick();
  sample->valid = sample->valid && good && sample->sequence != 0U &&
    (uint32_t)(now - sample->updated_ms) < SAMPLE_STALE_MS &&
    (uint32_t)(now - diag_time) < DIAG_STALE_MS;
  return sample->valid;
}
static void PrintSample(void)
{
  AS5600_Sample sample;
  bool valid = AS5600_GetSample(&sample);
  uint32_t mech = ((uint32_t)sample.raw * 45000U + 256U) / 512U;
  printf("AS5600 %s raw=%u, mech=%lu.%03lu deg, seq=%lu, age=%lu ms\r\n",
      valid ? "OK" : "INVALID/STALE", (unsigned)sample.raw,
      (unsigned long)(mech/1000U), (unsigned long)(mech%1000U),
      (unsigned long)sample.sequence, (unsigned long)(HAL_GetTick()-sample.updated_ms));
}
static uint32_t CyclesToHundredthUs(uint32_t cycles)
{
  return (uint32_t)(((uint64_t)cycles * 100000000ULL + SystemCoreClock/2U) / SystemCoreClock);
}
static void PrintStatus(void)
{
  /* Existing one-line-per-main-loop reporting avoids a long console burst. */
  unsigned line = status_line++;
  if (line == 1U) PrintSample();
  else if (line == 2U) {
    uint32_t transfer = CyclesToHundredthUs(transfer_cycles);
    uint32_t maximum = CyclesToHundredthUs(max_transfer_cycles);
    printf("AS5600 DMA: source=%s, state=%u, transfers=%lu, transfer=%lu.%02lu/max %lu.%02lu us, conf=0x%04X\r\n",
        sample_timer && (sample_timer->Instance->CR1 & TIM_CR1_CEN) ? "TIM1" : "main",
        (unsigned)state, (unsigned long)transfers,
        (unsigned long)(transfer/100U), (unsigned long)(transfer%100U),
        (unsigned long)(maximum/100U), (unsigned long)(maximum%100U), (unsigned)configuration);
  } else if (line == 3U) {
    printf("AS5600 errors: i2c=%lu, sensor=%lu, timeout=%lu, status=0x%02X, MD=%u ML=%u MH=%u\r\n",
        (unsigned long)i2c_errors, (unsigned long)sensor_errors, (unsigned long)timeouts,
        (unsigned)diagnostic, !!(diagnostic & 0x20U), !!(diagnostic & 0x10U), !!(diagnostic & 0x08U));
  } else if (line == 4U) {
    printf("AS5600 first fault: %s, HAL=0x%08lX, ISR=0x%08lX, elapsed=%lu us\r\n",
        first_fault ? first_fault : "none", (unsigned long)fault_error,
        (unsigned long)fault_isr, (unsigned long)(fault_cycles/(SystemCoreClock/1000000U)));
  } else {
    uint32_t interval = CyclesToHundredthUs(tim_interval_max_cycles);
    uint32_t age = CyclesToHundredthUs(tim_age_max_cycles);
    printf("AS5600 TIM: interval_max=%lu.%02lu us, age_max=%lu.%02lu us (read reference, after first TIM sample)\r\n",
        (unsigned long)(interval/100U), (unsigned long)(interval%100U),
        (unsigned long)(age/100U), (unsigned long)(age%100U));
  }
  if (line >= 5U) status_line = 0U;
}
static bool Equals(const char *text, const char *expected)
{
  while (isspace((unsigned char)*text)) text++;
  while (*expected) { if (tolower((unsigned char)*text) != *expected++) return false; text++; }
  while (isspace((unsigned char)*text)) text++;
  return *text == '\0';
}
bool AS5600_ProcessCommand(const char *command)
{
  if (command == NULL) return false;
  if (Equals(command, "angle") || Equals(command, "angle start")) {
    streaming = true; report_ms = HAL_GetTick() - 100U;
    printf("AS5600 angle display started\r\n");
  } else if (Equals(command, "angle stop")) {
    streaming = false;
    printf("Angle display stopped; DMA acquisition continues\r\n");
  } else if (Equals(command, "angle status")) status_line = 1U;
  else return false;
  return true;
}
/* Main only: reject callbacks before stopping DMA and resetting the peripheral.
 * Reset is bounded; a physically stuck bus remains invalid and retries later.
 * No SCL recovery pulses or OTP writes. */
void AS5600_Pause(void)
{
  if (encoder_i2c == NULL || !MotorControl_IsStopped()) return;
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  state = OFF; latest.valid = false; diagnostic_ok = false; configured = false;
  previous_timer_angle = false;
  CLEAR_BIT(encoder_i2c->Instance->CR1, I2C_CR1_TXIE | I2C_CR1_RXIE |
      I2C_CR1_ADDRIE | I2C_CR1_NACKIE | I2C_CR1_STOPIE | I2C_CR1_TCIE |
      I2C_CR1_ERRIE | I2C_CR1_TXDMAEN | I2C_CR1_RXDMAEN);
  __set_PRIMASK(mask);
  (void)HAL_DMA_Abort(encoder_i2c->hdmarx);
  (void)HAL_I2C_DeInit(encoder_i2c);
}
static bool Configure(void)
{
  /* Main only, stopped PWM. Preserve output/reserved bits; no OTP burn.
   * NOM, HYST off, SF=11 (2x), FTH=0 (fixed slow filter), WD off.
   * DS000365 CONF bits 13:8 and 3:0. Read back before trusting samples. */
  uint8_t data[2];
  if (HAL_I2C_Mem_Read(encoder_i2c, DEVICE_ADDRESS, REG_CONF,
      I2C_MEMADD_SIZE_8BIT, data, 2U, 10U) != HAL_OK) return false;
  uint16_t conf = ((uint16_t)data[0] << 8U) | data[1];
  conf = (conf & ~0x3F0FU) | 0x0300U;
  data[0] = (uint8_t)(conf >> 8U); data[1] = (uint8_t)conf;
  if (HAL_I2C_Mem_Write(encoder_i2c, DEVICE_ADDRESS, REG_CONF,
      I2C_MEMADD_SIZE_8BIT, data, 2U, 10U) != HAL_OK) return false;
  if (HAL_I2C_Mem_Read(encoder_i2c, DEVICE_ADDRESS, REG_CONF,
      I2C_MEMADD_SIZE_8BIT, data, 2U, 10U) != HAL_OK) return false;
  configuration = ((uint16_t)data[0] << 8U) | data[1];
  if (configuration != conf) return false;
  HAL_Delay(1U); /* Let the new filter settle before requesting angles. */
  configured = true; launched = false;
  return true;
}
void AS5600_Resume(void)
{
  if (encoder_i2c == NULL || state != OFF || !MotorControl_IsStopped()) return;
  state = RECOVERING;
  I2C1_Reinitialize(); /* Peripheral settings belong to CubeMX. */
  if (Configure()) state = IDLE;
  else {
    RecordFault("I2C configuration failed"); i2c_errors++; Fail();
  }
}
void AS5600_Task(void)
{
  if (encoder_i2c == NULL || state == OFF) {
    if (status_line) PrintStatus();
    return;
  }
  if (state == REQUEST && Expired()) {
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    if (state == REQUEST && Expired()) {
      RecordFault("main transfer timeout"); timeouts++; Fail();
    }
    __set_PRIMASK(mask);
  }
  uint32_t now = HAL_GetTick();
  if (state == FAILED && MotorControl_IsStopped()) {
    AS5600_Pause(); state = RECOVERING; recovery_ms = now;
  }
  if (state == RECOVERING && MotorControl_IsStopped() &&
      (uint32_t)(now-recovery_ms) >= RECOVERY_DELAY_MS) {
    state = OFF; AS5600_Resume();
  }
  /* During PWM, TIM1/ADC owns requests. Main never starts timer/PWM itself. */
  if (sample_timer && !(sample_timer->Instance->CR1 & TIM_CR1_CEN) && now != fallback_ms) {
    fallback_ms = now; AS5600_Tick();
  }
  if (status_line) { PrintStatus(); return; }
  if (streaming && (uint32_t)(now-report_ms) >= 100U) { report_ms = now; PrintSample(); }
}
void AS5600_Init(I2C_HandleTypeDef *i2c, TIM_HandleTypeDef *timer)
{
  state = OFF;
  if (timer == NULL || timer->Instance != TIM1 ||
      i2c == NULL || i2c->Instance != I2C1 || i2c->hdmarx == NULL ||
      i2c->Init.AddressingMode != I2C_ADDRESSINGMODE_7BIT ||
      i2c->hdmarx->Init.Request != DMA_REQUEST_I2C1_RX ||
      i2c->hdmarx->Init.Mode != DMA_NORMAL ||
      i2c->hdmarx->Init.Direction != DMA_PERIPH_TO_MEMORY ||
      i2c->hdmarx->Init.PeriphDataAlignment != DMA_PDATAALIGN_BYTE ||
      i2c->hdmarx->Init.MemDataAlignment != DMA_MDATAALIGN_BYTE ||
      i2c->hdmarx->Init.PeriphInc != DMA_PINC_DISABLE ||
      i2c->hdmarx->Init.MemInc != DMA_MINC_ENABLE) {
    printf("AS5600 I2C RX DMA configuration unsupported\r\n"); return;
  }
  encoder_i2c = i2c; sample_timer = timer;
  SET_BIT(CoreDebug->DEMCR, CoreDebug_DEMCR_TRCENA_Msk);
  SET_BIT(DWT->CTRL, DWT_CTRL_CYCCNTENA_Msk);
  /* Do not reset the shared DWT counter. Allow sensor power-up before reading. */
  HAL_Delay(10U);
  start_cycles = DWT->CYCCNT;
  if (Configure()) state = IDLE;
  else { RecordFault("sensor configuration failed"); i2c_errors++; Fail(); }
  printf("AS5600 %s; use 'angle' or 'angle status'\r\n",
      configured ? "acquisition ready" : "configuration failed, retrying while stopped");
}
