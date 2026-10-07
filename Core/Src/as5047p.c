#include "as5047p.h"
#include "irq_trace.h"
#include "motor_control.h"
#include "motor_control_config.h"
#include <ctype.h>
#include <stdio.h>

#define REG_ANGLE 0x3FFFU
#define REG_DIAG 0x3FFCU
#define REG_ERROR 0x0001U
#define REG_NONE 0xFFFFU
#define TWO_PI 6.2831853071795864769f
/* CubeMXで設定する固定構成。core/APB2/TIM8=160 MHz、SPI=5 MHz。
 * タイマーとDWTの1 tickが等しいので、フレームごとの除算は不要。 */
#define TIMER_HZ 160000000U
#define FRAME_CYCLES 8000U
#define CS_FALL_CYCLES 6400U
#define TX_CYCLES 6560U
#define WIRE_CYCLES (16U * 32U)
#define COMMIT_GUARD_CYCLES 800U /* 次の送信まで5 us以上残す */
typedef enum { OFF, RUNNING, FAILED } TransferState;
static volatile TransferState state;
static SPI_HandleTypeDef *encoder_spi;
static TIM_HandleTypeDef *sample_timer;
static DMA_HandleTypeDef *tx_handle;
static DMA_Channel_TypeDef *rx_dma, *tx_dma;
static uint32_t rx_clear, tx_clear, rx_ht, rx_tc, rx_te, tx_tc, tx_te;
static bool owned;
/* 初期化はCubeMX/HAL。起動後のTIM8/SPI1/DMAはこのモジュールが専有する。
 * TXは固定アドレス1語、RXは2語。DMAが触る領域はvolatileで保持する。 */
static volatile uint16_t tx_word, rx_words[2];
static uint16_t response_address;
static uint8_t expected_slot;
static uint32_t next_launch, previous_launch;
static volatile uint32_t last_received, last_received_ms;
static volatile AS5047P_Sample samples[2];
static volatile uint8_t published;
#define latest samples[published]
static volatile bool diagnostic_ok;
static bool read_error_next;
static volatile uint32_t diagnostic_ms;
static volatile uint16_t diagnostic, error_flags;
static volatile uint32_t transfers, spi_errors, parity_errors, sensor_errors, timeouts, overruns;
static volatile uint32_t max_receive_cycles, max_publish_cycles;
static const char *volatile first_fault;
static volatile uint32_t fault_sr, fault_dma_flags, fault_counter;
static uint16_t frame_command, frame_previous_command, frame_response;
static uint16_t previous_command;
static uint32_t frame_received;
static volatile uint16_t fault_command, fault_previous_command, fault_response;
static volatile uint32_t fault_frame, fault_request, fault_received;
static uint32_t report_ms;
static bool streaming;
static unsigned status_line;

static bool OddParity(uint16_t word)
{
  word ^= word >> 8; word ^= word >> 4; word ^= word >> 2; word ^= word >> 1;
  return (word & 1U) != 0U;
}
static uint16_t ReadCommand(uint16_t address)
{
  uint16_t word = address | 0x4000U;
  return OddParity(word) ? word | 0x8000U : word;
}
/* 以下の状態変更は短い排他区間内から呼ぶ。異常時だけ周辺停止を行う。 */
static void RecordFault(const char *reason)
{
  if (first_fault) return;
  fault_sr = encoder_spi->Instance->SR;
  fault_dma_flags = encoder_spi->hdmarx->DmaBaseAddress->ISR;
  fault_counter = sample_timer->Instance->CNT;
  fault_command = frame_command;
  fault_previous_command = frame_previous_command;
  fault_response = frame_response;
  fault_frame = transfers;
  fault_request = previous_launch;
  fault_received = frame_received;
  first_fault = reason;
}
static bool StopStream(void)
{
  TIM_TypeDef *tim = sample_timer->Instance;
  CLEAR_BIT(tim->DIER, TIM_DIER_CC2DE);
  CLEAR_BIT(tim->CR1, TIM_CR1_CEN); /* CSを現在の状態で保持し、追加送信を止める */
  CLEAR_BIT(tx_dma->CCR, DMA_CCR_EN);
  CLEAR_BIT(encoder_spi->Instance->CR2, SPI_CR2_ERRIE);
  uint32_t began = DWT->CYCCNT;
  bool idle = true;
  while (encoder_spi->Instance->SR & SPI_SR_BSY) {
    if ((uint32_t)(DWT->CYCCNT - began) >= TIMER_HZ / 100000U) {
      idle = false;
      break;
    }
  }
  CLEAR_BIT(encoder_spi->Instance->CR2, SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
  CLEAR_BIT(rx_dma->CCR, DMA_CCR_EN);
  began = DWT->CYCCNT;
  while ((uint32_t)(DWT->CYCCNT - began) < TIMER_HZ / 2000000U) { }
  /* PA15はAFなのでGPIO BSRRではなくOIS1+OSSIでCSをHighへ戻す。
   * CC1Eは保持する。BSY異常でも待ち続けずフレームを破棄する。 */
  CLEAR_BIT(tim->BDTR, TIM_BDTR_MOE);
  encoder_spi->hdmarx->DmaBaseAddress->IFCR = rx_clear;
  tx_handle->DmaBaseAddress->IFCR = tx_clear;
  latest.valid = false;
  diagnostic_ok = false;
  return idle;
}
static void Fail(const char *reason)
{
  RecordFault(reason);
  state = FAILED;
  (void)StopStream();
}
void AS5047P_Watchdog(void)
{
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  if (state == RUNNING &&
      ((uint32_t)(DWT->CYCCNT - last_received) >
         (TIMER_HZ / 1000000U) * MOTOR_CONTROL_ENCODER_TIMEOUT_US ||
       (uint32_t)(HAL_GetTick() - last_received_ms) >= MOTOR_CONTROL_ENCODER_STALE_MS ||
       !(sample_timer->Instance->CR1 & TIM_CR1_CEN) ||
       !(rx_dma->CCR & DMA_CCR_EN) || !(tx_dma->CCR & DMA_CCR_EN))) {
    timeouts++;
    Fail("stream stopped/timeout");
  }
  __set_PRIMASK(mask);
}

/* next_launchはCENを立てる直前のDWTを基点とする保守的な要求時刻。
 * 遅れたIRQの時刻から逆算しないので、古い値に新しい時刻を付けない。
 * 毎フレーム必ずHT/TCを1つずつ消費し、欠落時は再同期まで無効にする。 */
static bool InFrameWindow(uint32_t now)
{
  uint32_t age = now - next_launch;
  return age >= WIRE_CYCLES && age < FRAME_CYCLES - COMMIT_GUARD_CYCLES;
}
static void ReceiveFrame(void)
{
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  if (state != RUNNING) { __set_PRIMASK(mask); return; }
  uint32_t now = DWT->CYCCNT;
  uint32_t flags = encoder_spi->hdmarx->DmaBaseAddress->ISR;
  uint32_t expected = expected_slot == 0U ? rx_ht : rx_tc;
  uint32_t left = rx_dma->CNDTR;
  if (!InFrameWindow(now) || (flags & (rx_ht | rx_tc)) != expected ||
      !(tx_handle->DmaBaseAddress->ISR & tx_tc) ||
      (expected_slot == 0U ? left != 1U : (left != 2U && left != 0U))) {
    overruns++;
    Fail("RX frame lost/late");
    __set_PRIMASK(mask);
    return;
  }
  uint16_t response = rx_words[expected_slot];
  frame_response = response;
  frame_command = tx_word;
  frame_previous_command = previous_command;
  frame_received = now;
  uint16_t sent_address = tx_word & 0x3FFFU;
  uint16_t address = response_address;
  uint32_t request = previous_launch;
  encoder_spi->hdmarx->DmaBaseAddress->IFCR = rx_clear;
  tx_handle->DmaBaseAddress->IFCR = tx_clear;
  last_received = now;
  last_received_ms = HAL_GetTick();
  __set_PRIMASK(mask);

  /* 浮動小数点換算は割り込みを許可して実行する。公開は最後に一括で行う。 */
  uint16_t raw = response & 0x3FFFU;
  AS5047P_Sample sample = {0};
  sample.raw = raw;
  sample.mechanical_rad = (float)raw * (TWO_PI / 16384.0f);
  sample.electrical_rad = (float)(((uint32_t)raw * MOTOR_CONTROL_POLE_PAIRS) & 0x3FFFU) * (TWO_PI / 16384.0f);
  sample.request_cycles = request;
  sample.received_cycles = now;

  mask = __get_PRIMASK(); __disable_irq();
  if (state != RUNNING) { __set_PRIMASK(mask); return; }
  uint32_t commit = DWT->CYCCNT;
  /* ADC等に中断され、次のTXへ近づいた場合はコマンドを書き換えない。 */
  if (!InFrameWindow(commit) ||
      (encoder_spi->hdmarx->DmaBaseAddress->ISR & (rx_ht | rx_tc | rx_te)) ||
      (tx_handle->DmaBaseAddress->ISR & tx_te)) {
    overruns++;
    Fail("RX processing deadline");
    __set_PRIMASK(mask);
    return;
  }
  transfers++;
  if (now - next_launch > max_receive_cycles) max_receive_cycles = now - next_launch;
  bool publish = false;
  if (address == REG_NONE) {
    /* 開始/復旧後の最初の応答は、要求との対応がないので必ず捨てる。 */
  } else if (OddParity(response)) {
    RecordFault("response parity"); parity_errors++;
    latest.valid = false; diagnostic_ok = false; read_error_next = true;
  } else if (address == REG_ERROR) {
    error_flags = raw; diagnostic_ok = false; latest.valid = false;
  } else if (response & 0x4000U) {
    RecordFault("sensor error flag"); sensor_errors++;
    latest.valid = false; diagnostic_ok = false; read_error_next = true;
  } else if (address == REG_DIAG) {
    diagnostic = raw; diagnostic_ms = HAL_GetTick();
    diagnostic_ok = (raw & 0x0F00U) == 0x0100U; /* MISO固定Lowも検出 */
    if (!diagnostic_ok) {
      RecordFault("sensor diagnostic"); sensor_errors++; latest.valid = false;
    }
  } else if (address == REG_ANGLE) {
    sample.updated_ms = HAL_GetTick();
    sample.sequence = latest.sequence + 1U;
    sample.valid = diagnostic_ok;
    publish = true;
  }

  uint16_t next_address = REG_ANGLE;
  if (read_error_next) {
    if (sent_address != REG_ERROR) next_address = REG_ERROR;
    read_error_next = false;
  } else if ((!diagnostic_ok ||
              (uint32_t)(HAL_GetTick() - diagnostic_ms) >= MOTOR_CONTROL_ENCODER_DIAG_PERIOD_MS) &&
             sent_address != REG_DIAG) next_address = REG_DIAG;
  /* TX完了を確認済みで、次の送信まで5us以上ある区間でのみ1語を更新する。 */
  tx_word = ReadCommand(next_address);
  __DMB();
  response_address = sent_address;
  previous_command = frame_command;
  previous_launch = next_launch;
  next_launch += FRAME_CYCLES;
  expected_slot ^= 1U;
  uint8_t next = published ^ 1U;
  if (publish) samples[next] = sample;
  __DMB();
  uint32_t finished = DWT->CYCCNT;
  if (finished - previous_launch > max_publish_cycles) max_publish_cycles = finished - previous_launch;
  /* 万一、排他区間自体が締切を越えた場合も角度を公開しない。 */
  if ((uint32_t)(finished - commit) >= COMMIT_GUARD_CYCLES) {
    overruns++; Fail("RX commit deadline");
  } else if (publish) {
    published = next;
  }
  __set_PRIMASK(mask);
  if (publish && state == RUNNING) IrqTrace_Event(TRACE_PUBLISH);
}
bool AS5047P_DMA_IRQHandler(DMA_HandleTypeDef *dma)
{
  if (!owned || (dma != encoder_spi->hdmarx && dma != tx_handle)) return false;
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  if (state != RUNNING) {
    dma->DmaBaseAddress->IFCR = dma == tx_handle ? tx_clear : rx_clear;
  } else if ((encoder_spi->hdmarx->DmaBaseAddress->ISR & rx_te) ||
             (tx_handle->DmaBaseAddress->ISR & tx_te)) {
    spi_errors++; Fail("DMA transfer error");
  }
  bool receive = state == RUNNING && dma == encoder_spi->hdmarx &&
    (dma->DmaBaseAddress->ISR & (rx_ht | rx_tc));
  __set_PRIMASK(mask);
  if (receive) ReceiveFrame();
  return true;
}
bool AS5047P_SPI_IRQHandler(SPI_HandleTypeDef *spi)
{
  if (!owned || spi != encoder_spi) return false;
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  if (state == RUNNING && (spi->Instance->SR & (SPI_SR_OVR | SPI_SR_MODF | SPI_SR_FRE))) {
    spi_errors++; Fail("SPI status error");
  }
  __set_PRIMASK(mask);
  return true;
}
bool AS5047P_GetSample(AS5047P_Sample *sample)
{
  if (sample == NULL) return false;
  AS5047P_Watchdog();
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  *sample = latest;
  bool good = state == RUNNING && diagnostic_ok;
  uint32_t diag_time = diagnostic_ms;
  __set_PRIMASK(mask);
  uint32_t now = HAL_GetTick();
  sample->valid = sample->valid && good && sample->sequence != 0U &&
    (uint32_t)(now - sample->updated_ms) < MOTOR_CONTROL_ENCODER_STALE_MS &&
    (uint32_t)(now - diag_time) < MOTOR_CONTROL_ENCODER_DIAG_STALE_MS;
  return sample->valid;
}
static void PrintSample(void)
{
  AS5047P_Sample sample;
  bool valid = AS5047P_GetSample(&sample);
  /* 360000/16384を約分して、14bit全域でuint32_tの乗算を溢れさせない。 */
  uint32_t mech = ((uint32_t)sample.raw * 45000U + 1024U) / 2048U;
  uint32_t elec = ((((uint32_t)sample.raw * MOTOR_CONTROL_POLE_PAIRS) & 0x3FFFU) * 45000U + 1024U) / 2048U;
  printf("AS5047P %s raw=%u, mech=%lu.%03lu deg, elec_uncal=%lu.%03lu deg, seq=%lu, age=%lu ms\r\n",
         valid ? "OK" : "INVALID/STALE", (unsigned int)sample.raw,
         (unsigned long)(mech/1000U), (unsigned long)(mech%1000U),
         (unsigned long)(elec/1000U), (unsigned long)(elec%1000U), (unsigned long)sample.sequence,
         (unsigned long)(HAL_GetTick() - sample.updated_ms));
}
static void PrintStatus(void)
{
  /* FOCのmain watchdogを妨げないよう整数整形・1 main周期1行に分割。 */
  unsigned line = status_line++;
  if (line == 1U) PrintSample();
  else if (line == 2U) {
    uint32_t rx = (max_receive_cycles*5U+4U)/8U;
    uint32_t commit = (max_publish_cycles*5U+4U)/8U;
    printf("Encoder DMA: source=TIM8, state=%u, frame_hz=20000, transfers=%lu, rx_delay_max=%lu.%02lu us, commit_delay_max=%lu.%02lu us\r\n",
         (unsigned int)state, (unsigned long)transfers,
         (unsigned long)(rx/100U), (unsigned long)(rx%100U),
         (unsigned long)(commit/100U), (unsigned long)(commit%100U));
  } else if (line == 3U) printf("Encoder errors: spi=%lu, parity=%lu, sensor=%lu, timeout=%lu, overruns=%lu, diag=0x%04X, errfl=0x%04X\r\n",
         (unsigned long)spi_errors, (unsigned long)parity_errors, (unsigned long)sensor_errors,
         (unsigned long)timeouts, (unsigned long)overruns, (unsigned int)diagnostic, (unsigned int)error_flags);
  else if (line == 4U) printf("Angle display: elec_uncal is before direction/offset correction. FOC uses calibration; check 'cal status' for validity and Flash save status.\r\n");
  else if (line == 5U) printf("Encoder first fault: %s, SR=0x%08lX, DMA=0x%08lX, TIM8_CNT=%lu\r\n",
         first_fault ? first_fault : "none", (unsigned long)fault_sr,
         (unsigned long)fault_dma_flags, (unsigned long)fault_counter);
  else printf("Encoder fault frame: n=%lu, previous_cmd=0x%04X, sent_cmd=0x%04X, response=0x%04X, request_cycle=%lu, received_cycle=%lu\r\n",
         (unsigned long)fault_frame, (unsigned int)fault_previous_command,
         (unsigned int)fault_command, (unsigned int)fault_response,
         (unsigned long)fault_request, (unsigned long)fault_received);
  if (line >= 6U) status_line = 0U;
}
static bool Equals(const char *text, const char *expected)
{
  while (isspace((unsigned char)*text)) text++;
  while (*expected) { if (tolower((unsigned char)*text) != *expected++) return false; text++; }
  while (isspace((unsigned char)*text)) text++;
  return *text == '\0';
}
bool AS5047P_ProcessCommand(const char *command)
{
  if (command == NULL) return false;
  if (Equals(command,"angle trace") || Equals(command,"angle trace dump")) {
    if (!MotorControl_IsStopped()) { printf("Stop PWM before trace command\r\n"); return true; }
    if (Equals(command,"angle trace")) { IrqTrace_Arm(); printf("Trace armed for next FOC start\r\n"); }
    else IrqTrace_Dump();
    return true;
  }
  if (Equals(command, "angle") || Equals(command, "angle start")) {
    streaming = true; report_ms = HAL_GetTick() - 100U;
    printf("Angle display started: elec_uncal is before direction/offset correction (not calibration status). Check 'cal status'.\r\n");
  } else if (Equals(command, "angle stop")) {
    streaming = false;
    printf("Angle display stopped; DMA acquisition continues\r\n");
  } else if (Equals(command, "angle status")) status_line = 1U;
  else return false;
  return true;
}

/* main専用。ISRを停止してFIFOを排出し、パイプラインを最初から作り直す。 */
static bool StartStream(void)
{
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  state = OFF;
  bool idle = StopStream();
  __HAL_SPI_DISABLE(encoder_spi);
  for (unsigned i = 0; i < 4U && (encoder_spi->Instance->SR & SPI_SR_RXNE); i++)
    (void)encoder_spi->Instance->DR;
  __HAL_SPI_CLEAR_OVRFLAG(encoder_spi);
  __set_PRIMASK(mask);
  if (!idle || HAL_SPI_Init(encoder_spi) != HAL_OK) return false;

  mask = __get_PRIMASK(); __disable_irq();
  TIM_TypeDef *tim = sample_timer->Instance;
  tim->DIER = 0U;
  CLEAR_BIT(tim->CR2, TIM_CR2_CCDS);
  tim->CNT = 0U;
  tim->EGR = TIM_EGR_UG; /* ARPEのEnable/Disable双方に対応 */
  tim->SR = 0U;
  tim->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E;
  tx_word = ReadCommand(REG_DIAG);
  rx_words[0] = rx_words[1] = 0U;
  response_address = REG_NONE;
  previous_command = 0U;
  expected_slot = 0U;
  read_error_next = false;
  rx_dma->CPAR = tx_dma->CPAR = (uint32_t)&encoder_spi->Instance->DR;
  rx_dma->CMAR = (uint32_t)rx_words;
  tx_dma->CMAR = (uint32_t)&tx_word;
  rx_dma->CNDTR = 2U; tx_dma->CNDTR = 1U;
  encoder_spi->hdmarx->DmaBaseAddress->IFCR = rx_clear;
  tx_handle->DmaBaseAddress->IFCR = tx_clear;
  MODIFY_REG(rx_dma->CCR, DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE,
             DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE);
  MODIFY_REG(tx_dma->CCR, DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE, DMA_CCR_TEIE);
  __DMB();
  __HAL_SPI_ENABLE(encoder_spi);
  SET_BIT(rx_dma->CCR, DMA_CCR_EN);
  CLEAR_BIT(encoder_spi->Instance->CR2, SPI_CR2_TXDMAEN); /* TXはTIM8要求のみ */
  SET_BIT(encoder_spi->Instance->CR2, SPI_CR2_RXDMAEN | SPI_CR2_ERRIE);
  SET_BIT(tx_dma->CCR, DMA_CCR_EN);
  SET_BIT(tim->BDTR, TIM_BDTR_MOE); /* CNT=0なのでCS High */
  SET_BIT(tim->DIER, TIM_DIER_CC2DE);
  last_received_ms = HAL_GetTick();
  state = RUNNING;
  /* 基点取得からCENまでを排他化。基点は実際の起動より早いためageを過小評価しない。 */
  last_received = DWT->CYCCNT;
  SET_BIT(tim->CR1, TIM_CR1_CEN);
  next_launch = last_received + TX_CYCLES;
  previous_launch = next_launch;
  __set_PRIMASK(mask);
  return true;
}
void AS5047P_Task(void)
{
  if (!owned || state == OFF) return;
  AS5047P_Watchdog();
  /* 運転中の障害をmain側の即時復旧で隠さない。保護停止後にだけ再開する。 */
  if (state == FAILED && MotorControl_IsStopped() && !StartStream())
    printf("Encoder TIM8 recovery failed; reset required\r\n");
  uint32_t now = HAL_GetTick();
  if (status_line) { PrintStatus(); return; }
  if (streaming && (uint32_t)(now - report_ms) >= 100U) { report_ms = now; PrintSample(); }
}
void AS5047P_Init(SPI_HandleTypeDef *spi, TIM_HandleTypeDef *timer)
{
  state = OFF;
  if (spi == NULL || timer == NULL || spi->hdmarx == NULL || timer->hdma[TIM_DMA_ID_CC2] == NULL) {
    printf("Encoder TIM8 DMA configuration missing\r\n"); return;
  }
  DMA_HandleTypeDef *rx = spi->hdmarx, *tx = timer->hdma[TIM_DMA_ID_CC2];
  TIM_TypeDef *tim = timer->Instance;
  /* 対応構成を限定し、CubeMXの変更を黙って上書きしない。 */
  if (SystemCoreClock != TIMER_HZ || HAL_RCC_GetPCLK2Freq() != TIMER_HZ ||
      spi->Instance != SPI1 || spi->Init.Mode != SPI_MODE_MASTER ||
      spi->Init.Direction != SPI_DIRECTION_2LINES || spi->Init.DataSize != SPI_DATASIZE_16BIT ||
      spi->Init.CLKPolarity != SPI_POLARITY_LOW || spi->Init.CLKPhase != SPI_PHASE_2EDGE ||
      spi->Init.BaudRatePrescaler != SPI_BAUDRATEPRESCALER_32 || spi->Init.FirstBit != SPI_FIRSTBIT_MSB ||
      spi->Init.NSS != SPI_NSS_SOFT || spi->Init.NSSPMode != SPI_NSS_PULSE_DISABLE ||
      spi->Init.TIMode != SPI_TIMODE_DISABLE || spi->Init.CRCCalculation != SPI_CRCCALCULATION_DISABLE ||
      tim != TIM8 || tim->PSC != 0U || tim->ARR != FRAME_CYCLES - 1U || tim->RCR != 0U ||
      tim->CCR1 != CS_FALL_CYCLES || tim->CCR2 != TX_CYCLES ||
      (tim->CR1 & (TIM_CR1_CEN | TIM_CR1_DIR | TIM_CR1_CMS | TIM_CR1_OPM)) ||
      (tim->CCMR1 & (TIM_CCMR1_OC1M | TIM_CCMR1_OC2M | TIM_CCMR1_CC1S | TIM_CCMR1_CC2S)) != TIM_OCMODE_PWM1 ||
      !(tim->CR2 & TIM_CR2_OIS1) || !(tim->BDTR & TIM_BDTR_OSSI) ||
      (tim->BDTR & (TIM_BDTR_BKE | TIM_BDTR_BK2E | TIM_BDTR_DTG | TIM_BDTR_LOCK)) ||
      (tim->CCER & (TIM_CCER_CC1P | TIM_CCER_CC1NE)) ||
      rx->Init.Request != DMA_REQUEST_SPI1_RX || tx->Init.Request != DMA_REQUEST_TIM8_CH2 ||
      rx->Init.Mode != DMA_CIRCULAR || tx->Init.Mode != DMA_CIRCULAR ||
      rx->Init.Direction != DMA_PERIPH_TO_MEMORY || tx->Init.Direction != DMA_MEMORY_TO_PERIPH ||
      rx->Init.PeriphDataAlignment != DMA_PDATAALIGN_HALFWORD || tx->Init.PeriphDataAlignment != DMA_PDATAALIGN_HALFWORD ||
      rx->Init.MemDataAlignment != DMA_MDATAALIGN_HALFWORD || tx->Init.MemDataAlignment != DMA_MDATAALIGN_HALFWORD ||
      rx->Init.PeriphInc != DMA_PINC_DISABLE || tx->Init.PeriphInc != DMA_PINC_DISABLE ||
      rx->Init.MemInc != DMA_MINC_ENABLE || tx->Init.MemInc != DMA_MINC_DISABLE) {
    printf("Encoder TIM8 configuration unsupported; check CubeMX\r\n"); return;
  }
  encoder_spi = spi; sample_timer = timer; tx_handle = tx;
  rx_dma = rx->Instance; tx_dma = tx->Instance;
  rx_clear = __HAL_DMA_GET_GI_FLAG_INDEX(rx); tx_clear = __HAL_DMA_GET_GI_FLAG_INDEX(tx);
  rx_ht = __HAL_DMA_GET_HT_FLAG_INDEX(rx); rx_tc = __HAL_DMA_GET_TC_FLAG_INDEX(rx);
  rx_te = __HAL_DMA_GET_TE_FLAG_INDEX(rx); tx_tc = __HAL_DMA_GET_TC_FLAG_INDEX(tx);
  tx_te = __HAL_DMA_GET_TE_FLAG_INDEX(tx);
  SET_BIT(CoreDebug->DEMCR, CoreDebug_DEMCR_TRCENA_Msk);
  SET_BIT(DWT->CTRL, DWT_CTRL_CYCCNTENA_Msk);
  SET_BIT(tim->CCER, TIM_CCER_CC1E);
  owned = true;
  (void)StopStream();
  HAL_Delay(10U);
  if (!StartStream()) printf("Encoder TIM8 start failed\r\n");
  uint32_t began = HAL_GetTick();
  while (latest.sequence == 0U && state != OFF && (uint32_t)(HAL_GetTick() - began) < 30U)
    AS5047P_Task();
  status_line = 1U;
  while (status_line) PrintStatus(); /* 起動時はモーター停止中 */
}
/* Flash保存前後のmain専用。モーター停止中にだけ呼ぶ。 */
void AS5047P_Pause(void)
{
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  if (owned) { state = OFF; (void)StopStream(); }
  __set_PRIMASK(mask);
}
void AS5047P_Resume(void)
{
  if (owned) state = FAILED; /* mainのStartStreamで診断からやり直す */
}
