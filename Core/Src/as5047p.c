#include "as5047p.h"
#include "main.h"
#include "irq_trace.h"
#include "motor_control.h"
#include "motor_control_config.h"
#include <ctype.h>
#include <stdio.h>

#define REG_ANGLE 0x3FFFU
#define REG_DIAG 0x3FFCU
#define REG_ERROR 0x0001U
#define TWO_PI 6.2831853071795864769f
typedef enum { OFF, IDLE, REQUEST, RESPONSE, FAILED, RECOVERING } TransferState;
static SPI_HandleTypeDef *encoder_spi;
static TIM_HandleTypeDef *sample_timer;
static DMA_Channel_TypeDef *rx_dma, *tx_dma;
static uint32_t rx_clear, tx_clear, rx_tc, rx_te, tx_te;
/* 初期化はCubeMX/HAL、転送中のSPIとDMAはこのモジュールが専有する。
 * HALの転送API/IRQ/Abortと混用しない（HALのStateは転送状態を表さない）。 */
static bool fast_owned;
static void FrameComplete(void);
static void StopDma(void)
{
  CLEAR_BIT(encoder_spi->Instance->CR2, SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN | SPI_CR2_ERRIE);
  CLEAR_BIT(rx_dma->CCR, DMA_CCR_EN);
  CLEAR_BIT(tx_dma->CCR, DMA_CCR_EN);
  encoder_spi->hdmarx->DmaBaseAddress->IFCR = rx_clear;
  encoder_spi->hdmatx->DmaBaseAddress->IFCR = tx_clear;
}
/* DMAのTCは最後のSCK終了とは限らない。BSYを確認してからCSを解放する。
 * ISRではSysTickを待てないためDWTで上限を設け、異常時にも永久待ちしない。 */
static bool WaitIdle(void)
{
  uint32_t began = DWT->CYCCNT;
  while (encoder_spi->Instance->SR & SPI_SR_BSY) {
    if ((uint32_t)(DWT->CYCCNT - began) > SystemCoreClock / 100000U) return false;
  }
  return true;
}
static volatile TransferState state;
/* DMAの送受信領域は完了まで保持する。スタック上には置かない。 */
static uint16_t tx_word, rx_word, address;
/* DMA ISRより高優先度の将来のFOC ISRも、公開済みの一組だけを読む。 */
static volatile AS5047P_Sample samples[2];
static volatile uint8_t published;
#define latest samples[published]
static volatile bool diagnostic_ok, read_error_next;
static volatile uint32_t diagnostic_ms, start_cycles, last_start_cycles;
static volatile uint32_t transfers, spi_errors, parity_errors, sensor_errors, timeouts, missed;
static volatile uint32_t transfer_cycles, max_transfer_cycles, interval_cycles, max_interval_cycles, max_launch_cycles;
static volatile uint16_t diagnostic, error_flags;
static uint32_t fallback_ms, report_ms;
static bool streaming;
/* 復旧で消える周辺状態を最初の異常時だけ保存する。ISRでは文字列整形しない。
 * state: 2=要求フレーム、3=応答フレーム。残数0かつTCありならDMA転送は完了し、
 * 完了ISRの処理待ちである可能性を切り分けられる。リセットで記録をクリアする。 */
static const char *volatile first_fault;
static volatile uint32_t fault_state, fault_cycles, fault_sr, fault_rx_left, fault_tx_left, fault_dma_flags;
static void RecordFault(const char *reason)
{
  uint32_t mask=__get_PRIMASK(); __disable_irq();
  if (!first_fault) {
    fault_state=state; fault_cycles=DWT->CYCCNT-start_cycles;
    fault_sr=encoder_spi->Instance->SR;
    fault_rx_left=rx_dma->CNDTR; fault_tx_left=tx_dma->CNDTR;
    fault_dma_flags=encoder_spi->hdmarx->DmaBaseAddress->ISR;
    first_fault=reason;
  }
  __set_PRIMASK(mask);
}


static bool OddParity(uint16_t word)
{
  word ^= word >> 8; word ^= word >> 4; word ^= word >> 2; word ^= word >> 1;
  return (word & 1U) != 0U;
}
static void DelayUs(void)
{
  uint32_t start = DWT->CYCCNT;
  while ((uint32_t)(DWT->CYCCNT - start) < SystemCoreClock / 1000000U) { }
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
static void StartFrame(uint16_t word)
{
  /* EN=0でのみCNDTRを書き換える。前回のフラグも次の転送前に消去する。
   * RXを先に準備し、最後にTX要求を許可して最初の受信データを取りこぼさない。 */
  StopDma();
  tx_word = word;
  rx_dma->CNDTR = 1U;
  tx_dma->CNDTR = 1U;
  SPI1_SS_GPIO_Port->BSRR = (uint32_t)SPI1_SS_Pin << 16U;
  __DMB(); /* DMAが読むRAMへの書き込みを、DMA起動より先に完了させる。 */
  SET_BIT(rx_dma->CCR, DMA_CCR_EN);
  SET_BIT(encoder_spi->Instance->CR2, SPI_CR2_RXDMAEN | SPI_CR2_ERRIE);
  DelayUs(); /* AS5047PのCS setup。クロック速度・CS待ち時間は従来のまま。 */
  SET_BIT(tx_dma->CCR, DMA_CCR_EN);
  IrqTrace_Event(TRACE_LAUNCH);
  SET_BIT(encoder_spi->Instance->CR2, SPI_CR2_TXDMAEN);
}
/* RXのTCだけで正常完了を通知。1ワードなのでHTとTXのTC割り込みは不要。
 * TX/RXのTEは両方監視し、異常時は要求を止めてmainで復旧する。 */
bool AS5047P_DMA_IRQHandler(DMA_HandleTypeDef *dma)
{
  if (!fast_owned || (dma != encoder_spi->hdmarx && dma != encoder_spi->hdmatx)) return false;
  uint32_t rflags = encoder_spi->hdmarx->DmaBaseAddress->ISR;
  uint32_t tflags = encoder_spi->hdmatx->DmaBaseAddress->ISR;
  if ((rflags & rx_te) || (tflags & tx_te)) {
    RecordFault("DMA transfer error"); StopDma(); spi_errors++; Fail();
  } else if (dma == encoder_spi->hdmarx && (rflags & rx_tc)) {
    StopDma();
    if (!WaitIdle()) { RecordFault("SPI BSY timeout"); timeouts++; Fail(); }
    else FrameComplete();
  } else {
    dma->DmaBaseAddress->IFCR = dma == encoder_spi->hdmarx ? rx_clear : tx_clear;
  }
  return true;
}
bool AS5047P_SPI_IRQHandler(SPI_HandleTypeDef *spi)
{
  if (!fast_owned || spi != encoder_spi) return false;
  if (spi->Instance->SR & (SPI_SR_OVR | SPI_SR_MODF | SPI_SR_FRE)) {
    RecordFault("SPI status error"); StopDma(); spi_errors++; Fail();
  }
  return true;
}

static void StartRead(void)
{
  if (state != IDLE) {
    if (state == REQUEST || state == RESPONSE) {
      missed++;
      if (Expired()) { RecordFault("TIM transfer timeout"); timeouts++; Fail(); }
    }
    return;
  }
  IrqTrace_Event(TRACE_READ);
  state = REQUEST;
  start_cycles = DWT->CYCCNT;
  if (transfers != 0U) {
    interval_cycles = start_cycles - last_start_cycles;
    if (interval_cycles > max_interval_cycles) max_interval_cycles = interval_cycles;
  }
  last_start_cycles = start_cycles;
  if (read_error_next) { address = REG_ERROR; read_error_next = false; }
  else if (!diagnostic_ok || (uint32_t)(HAL_GetTick() - diagnostic_ms) >= MOTOR_CONTROL_ENCODER_DIAG_PERIOD_MS)
    address = REG_DIAG;
  else address = REG_ANGLE;
  uint16_t command = 0x4000U | address;
  if (OddParity(command)) command |= 0x8000U;
  StartFrame(command);
  uint32_t elapsed = DWT->CYCCNT - start_cycles;
  if (elapsed > max_launch_cycles) max_launch_cycles = elapsed;
}
void AS5047P_Tick(void)
{
  if (encoder_spi != NULL && state != OFF) StartRead();
}
static void FrameComplete(void)
{
  if (state != REQUEST && state != RESPONSE) return;
  uint16_t response = rx_word;
  /* BSY解除を確認済み。各16bitの間でCSをHighに戻す。 */
  DelayUs();
  SPI1_SS_GPIO_Port->BSRR = SPI1_SS_Pin;
  DelayUs();
  if (state == REQUEST) {
    state = RESPONSE;
    StartFrame(0U); /* NOPが前フレームの要求に対応するデータを返す。 */
    return;
  }
  if (state != RESPONSE) return;
  uint32_t received = DWT->CYCCNT;
  transfer_cycles = received - start_cycles;
  if (transfer_cycles > max_transfer_cycles) max_transfer_cycles = transfer_cycles;
  transfers++;
  if (OddParity(response)) {
    RecordFault("response parity"); parity_errors++; latest.valid = false; diagnostic_ok = false;
  } else if (address == REG_ERROR) {
    error_flags = response & 0x3FFFU; diagnostic_ok = false;
  } else if ((response & 0x4000U) != 0U) {
    RecordFault("sensor error flag"); sensor_errors++; latest.valid = false; diagnostic_ok = false; read_error_next = true;
  } else if (address == REG_DIAG) {
    diagnostic = response & 0x3FFFU; diagnostic_ms = HAL_GetTick();
    /* LFを含めて確認し、MISO固定Lowを正常な角度0と誤認しない。 */
    diagnostic_ok = (diagnostic & 0x0F00U) == 0x0100U;
    if (!diagnostic_ok) { RecordFault("sensor diagnostic"); sensor_errors++; latest.valid = false; }
  } else {
    uint16_t raw = response & 0x3FFFU;
    uint8_t next = published ^ 1U;
    volatile AS5047P_Sample *sample = &samples[next];
    sample->raw = raw;
    /* エンコーダー増加方向が正。極対数倍して1電気回転内へ折り返す。 */
    sample->mechanical_rad = (float)raw * (TWO_PI / 16384.0f);
    sample->electrical_rad = (float)(((uint32_t)raw * MOTOR_CONTROL_POLE_PAIRS) & 0x3FFFU) * (TWO_PI / 16384.0f);
    sample->request_cycles = start_cycles; sample->received_cycles = received;
    sample->updated_ms = HAL_GetTick(); sample->sequence = latest.sequence + 1U;
    sample->valid = diagnostic_ok && state == RESPONSE;
    __DMB();
    published = next;
    IrqTrace_Event(TRACE_PUBLISH);
  }
  __DMB();
  if (state == RESPONSE) state = IDLE;
}
bool AS5047P_GetSample(AS5047P_Sample *sample)
{
  if (sample == NULL) return false;
  /* 角度と時刻を同じ更新番号の組としてコピーする短い排他区間。 */
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  *sample = latest;
  bool good = diagnostic_ok;
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
  printf("AS5047P %s raw=%u, mech=%.3f deg, elec_uncal=%.3f deg, seq=%lu, age=%lu ms\r\n",
         valid ? "OK" : "INVALID/STALE", (unsigned int)sample.raw,
         (double)sample.mechanical_rad * (360.0 / TWO_PI),
         (double)sample.electrical_rad * (360.0 / TWO_PI), (unsigned long)sample.sequence,
         (unsigned long)(HAL_GetTick() - sample.updated_ms));
}
static void PrintStatus(void)
{
  PrintSample();
  const double us = 1000000.0 / SystemCoreClock;
  printf("Encoder DMA: source=%s, transfers=%lu, transfer=%.2f/max %.2f us, interval=%.2f/max %.2f us, launch_max=%.2f us\r\n",
         (sample_timer != NULL && (sample_timer->Instance->CR1 & TIM_CR1_CEN)) ? "TIM1" : "main",
         (unsigned long)transfers, transfer_cycles * us, max_transfer_cycles * us,
         interval_cycles * us, max_interval_cycles * us, max_launch_cycles * us);
  printf("Encoder errors: spi=%lu, parity=%lu, sensor=%lu, timeout=%lu, busy_ticks=%lu, diag=0x%04X, errfl=0x%04X; displayed electrical angle is uncalibrated\r\n",
         (unsigned long)spi_errors, (unsigned long)parity_errors, (unsigned long)sensor_errors,
         (unsigned long)timeouts, (unsigned long)missed, (unsigned int)diagnostic, (unsigned int)error_flags);
  printf("Encoder first fault: %s, state=%lu, elapsed=%lu ns, SR=0x%08lX, RXleft=%lu, TXleft=%lu, DMA=0x%08lX\r\n",
      first_fault ? first_fault : "none", (unsigned long)fault_state,
      (unsigned long)((float)fault_cycles*(1e9f/(float)SystemCoreClock)),
      (unsigned long)fault_sr,(unsigned long)fault_rx_left,(unsigned long)fault_tx_left,
      (unsigned long)fault_dma_flags);
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
    printf("Angle display started (uncalibrated electrical angle)\r\n");
  } else if (Equals(command, "angle stop")) {
    streaming = false;
    printf("Angle display stopped; DMA acquisition continues\r\n");
  } else if (Equals(command, "angle status")) PrintStatus();
  else return false;
  return true;
}
void AS5047P_Task(void)
{
  if (encoder_spi == NULL || state == OFF) return;
  if ((state == REQUEST || state == RESPONSE) && Expired()) {
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    if ((state == REQUEST || state == RESPONSE) && Expired()) { RecordFault("main transfer timeout"); timeouts++; Fail(); }
    __set_PRIMASK(mask);
  }
  if (state == FAILED) {
    state = RECOVERING;
    /* DMAを停止してからSPIを再初期化。HAL_AbortはHAL転送と混用しないため使わない。
     * 正常時は触らないFIFO/OVRもここで排出・解除する。再開不能ならOFFを維持する。 */
    StopDma();
    bool idle = WaitIdle();
    __HAL_SPI_DISABLE(encoder_spi);
    for (unsigned int i = 0; i < 4U && (encoder_spi->Instance->SR & SPI_SR_RXNE); i++) {
      (void)encoder_spi->Instance->DR;
    }
    __HAL_SPI_CLEAR_OVRFLAG(encoder_spi);
    HAL_StatusTypeDef result = idle ? HAL_SPI_Init(encoder_spi) : HAL_ERROR;
    SPI1_SS_GPIO_Port->BSRR = SPI1_SS_Pin;
    DelayUs();
    if (result == HAL_OK) __HAL_SPI_ENABLE(encoder_spi);
    state = result == HAL_OK ? IDLE : OFF;
    if (result != HAL_OK) printf("Encoder DMA recovery failed; reset required\r\n");
  }
  uint32_t now = HAL_GetTick();
  /* TIM1停止中の手回し観測。タイマ/PWMはここから開始しない。 */
  if ((sample_timer->Instance->CR1 & TIM_CR1_CEN) == 0U && now != fallback_ms) {
    fallback_ms = now; StartRead();
  }
  if (streaming && (uint32_t)(now - report_ms) >= 100U) { report_ms = now; PrintSample(); }
}
void AS5047P_Init(SPI_HandleTypeDef *spi, TIM_HandleTypeDef *timer)
{
  state = OFF;
  if (spi == NULL || timer == NULL || spi->hdmarx == NULL || spi->hdmatx == NULL) {
    printf("Encoder DMA configuration missing\r\n"); return;
  }
  /* 16bit・通常DMA専用。CubeMX設定が変わったら黙って不正転送せず停止する。 */
  if (spi->Init.DataSize != SPI_DATASIZE_16BIT || spi->Init.Mode != SPI_MODE_MASTER ||
      spi->Init.Direction != SPI_DIRECTION_2LINES || spi->Init.NSS != SPI_NSS_SOFT ||
      spi->Init.CRCCalculation != SPI_CRCCALCULATION_DISABLE ||
      spi->hdmarx->Init.Mode != DMA_NORMAL || spi->hdmatx->Init.Mode != DMA_NORMAL ||
      spi->hdmarx->Init.Direction != DMA_PERIPH_TO_MEMORY ||
      spi->hdmatx->Init.Direction != DMA_MEMORY_TO_PERIPH ||
      spi->hdmarx->Init.PeriphDataAlignment != DMA_PDATAALIGN_HALFWORD ||
      spi->hdmatx->Init.PeriphDataAlignment != DMA_PDATAALIGN_HALFWORD ||
      spi->hdmarx->Init.MemDataAlignment != DMA_MDATAALIGN_HALFWORD ||
      spi->hdmatx->Init.MemDataAlignment != DMA_MDATAALIGN_HALFWORD ||
      spi->hdmarx->Init.PeriphInc != DMA_PINC_DISABLE ||
      spi->hdmatx->Init.PeriphInc != DMA_PINC_DISABLE) {
    printf("Encoder fast DMA configuration unsupported\r\n"); return;
  }
  encoder_spi = spi; sample_timer = timer;
  rx_dma = spi->hdmarx->Instance; tx_dma = spi->hdmatx->Instance;
  rx_clear = __HAL_DMA_GET_GI_FLAG_INDEX(spi->hdmarx);
  tx_clear = __HAL_DMA_GET_GI_FLAG_INDEX(spi->hdmatx);
  rx_tc = __HAL_DMA_GET_TC_FLAG_INDEX(spi->hdmarx);
  rx_te = __HAL_DMA_GET_TE_FLAG_INDEX(spi->hdmarx);
  tx_te = __HAL_DMA_GET_TE_FLAG_INDEX(spi->hdmatx);
  StopDma();
  /* アドレスは固定。CubeMXが決めるDMAMUX、幅、優先度は変更しない。 */
  rx_dma->CPAR = tx_dma->CPAR = (uint32_t)&spi->Instance->DR;
  rx_dma->CMAR = (uint32_t)&rx_word; tx_dma->CMAR = (uint32_t)&tx_word;
  MODIFY_REG(rx_dma->CCR, DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE, DMA_CCR_TCIE | DMA_CCR_TEIE);
  MODIFY_REG(tx_dma->CCR, DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE, DMA_CCR_TEIE);
  fast_owned = true;
  __HAL_SPI_ENABLE(spi);
  /* DWTは既存処理と共用し、カウンタをリセットしない。 */
  SET_BIT(CoreDebug->DEMCR, CoreDebug_DEMCR_TRCENA_Msk);
  SET_BIT(DWT->CTRL, DWT_CTRL_CYCCNTENA_Msk);
  SPI1_SS_GPIO_Port->BSRR = SPI1_SS_Pin;
  HAL_Delay(10U);
  state = IDLE;
  uint32_t began = HAL_GetTick();
  while (latest.sequence == 0U && (uint32_t)(HAL_GetTick() - began) < 30U) AS5047P_Task();
  PrintStatus();
}

/* Flash書き込み前のmain専用。PWM停止中にだけ使用する。 */
void AS5047P_Pause(void)
{
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  if (!fast_owned) { __set_PRIMASK(mask); return; }
  state = OFF; StopDma();
  (void)WaitIdle();
  SPI1_SS_GPIO_Port->BSRR = SPI1_SS_Pin;
  latest.valid = false; diagnostic_ok = false;
  __set_PRIMASK(mask);
}
void AS5047P_Resume(void)
{
  /* 途中の応答は破棄し、既存のmain復旧処理でFIFOを排出する。 */
  if (fast_owned) state = FAILED;
}
