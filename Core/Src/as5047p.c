#include "as5047p.h"
#include "main.h"
#include "motor_control.h"
#include "motor_control_config.h"
#include <ctype.h>
#include <stdio.h>

#define REG_ANGLE 0x3FFFU
#define REG_DIAG 0x3FFCU
#define REG_ERROR 0x0001U
#define TWO_PI 6.2831853071795864769f
typedef enum { OFF, IDLE, REQUEST, RESPONSE, FAILED, RECOVERING, DECODING } TransferState;
static SPI_HandleTypeDef *encoder_spi;
static TIM_HandleTypeDef *sample_timer;
static DMA_Channel_TypeDef *rx_dma, *tx_dma;
static uint32_t rx_clear, tx_clear, rx_tc, rx_te, tx_te;
/* 初期化はCubeMX/HAL、転送中のSPIとDMAはこのモジュールが専有する。
 * HALの転送API/IRQ/Abortと混用しない（HALのStateは転送状態を表さない）。 */
static bool fast_owned;
static uint32_t cs_guard_cycles, cs_high_cycles;
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
static volatile uint32_t transfers, spi_errors, parity_errors, sensor_errors, timeouts, missed, decode_waits;
static volatile uint32_t transfer_cycles, max_transfer_cycles, interval_cycles, max_interval_cycles, max_launch_cycles;
static volatile uint16_t diagnostic, error_flags;
static uint32_t fallback_ms, report_ms;
static bool streaming;
static unsigned status_line;
#ifdef ENCODER_TIMING_PROBE
/* Investigation-only timestamps. No UART from ISR; freeze one DECODING wait
 * and its following publication/ADC use. Production builds exclude all hooks. */
static volatile bool probe_armed, probe_pending, probe_ready;
static volatile uint32_t probe_req_irq, probe_resp_irq, probe_decode, probe_adc;
static volatile uint32_t probe_received;
static volatile uint32_t probe_use_time, probe_use_seq, probe_use_request;
typedef struct {
  uint32_t request, req_irq, resp_irq, decode, adc, use_time, use_seq, use_request;
  uint32_t wait, ipsr, active_dma, active_adc, received, publish, new_seq;
  uint32_t next_use, next_seq, next_request, address;
} TimingProbe;
static volatile TimingProbe probe;
void AS5047P_TimingAdcBegin(void)
{
  if (probe_armed) probe_adc=DWT->CYCCNT;
}
static void ProbeWait(void)
{
  if (!probe_armed || address != REG_ANGLE || __get_IPSR() != (uint32_t)ADC1_2_IRQn+16U) return;
  probe.wait=DWT->CYCCNT;
  probe.request=start_cycles; probe.req_irq=probe_req_irq;
  probe.resp_irq=probe_resp_irq; probe.decode=probe_decode;
  probe.adc=probe_adc; probe.use_time=probe_use_time;
  probe.use_seq=probe_use_seq; probe.use_request=probe_use_request;
  probe.ipsr=__get_IPSR();
  probe.active_dma=NVIC_GetActive(DMA1_Channel2_IRQn);
  probe.active_adc=NVIC_GetActive(ADC1_2_IRQn);
  probe.address=address;
  probe.received=probe_received;
  probe_armed=false; probe_pending=true;
}
static void ProbeUse(const AS5047P_Sample *sample)
{
  if (__get_IPSR() != (uint32_t)ADC1_2_IRQn+16U) return;
  if (probe_armed) {
    probe_use_time=DWT->CYCCNT; probe_use_seq=sample->sequence;
    probe_use_request=sample->request_cycles;
  } else if (probe_pending && probe.publish) {
    probe.next_use=DWT->CYCCNT; probe.next_seq=sample->sequence;
    probe.next_request=sample->request_cycles;
    probe_pending=false; probe_ready=true;
  }
}
static void ProbeDump(void)
{
  if (!MotorControl_IsStopped()) { printf("TIMING stop PWM before dump\r\n"); return; }
  printf("TIMING status armed=%u pending=%u ready=%u hz=%lu\r\n",
      (unsigned)probe_armed,(unsigned)probe_pending,(unsigned)probe_ready,(unsigned long)SystemCoreClock);
  if (!probe_ready) return;
  printf("TIMING transfer %lu,%lu,%lu,%lu,%lu,%lu,%lu\r\n",
      (unsigned long)probe.request,(unsigned long)probe.req_irq,(unsigned long)probe.resp_irq,
      (unsigned long)probe.decode,(unsigned long)probe.received,(unsigned long)probe.publish,(unsigned long)probe.address);
  printf("TIMING wait %lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\r\n",
      (unsigned long)probe.adc,(unsigned long)probe.use_time,(unsigned long)probe.use_seq,
      (unsigned long)probe.use_request,(unsigned long)probe.wait,(unsigned long)probe.ipsr,
      (unsigned long)probe.active_dma,(unsigned long)probe.active_adc);
  printf("TIMING next %lu,%lu,%lu,%lu\r\n",(unsigned long)probe.new_seq,
      (unsigned long)probe.next_use,(unsigned long)probe.next_seq,(unsigned long)probe.next_request);
}
#endif
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
/* AS5047P DS000324 v2-00 Figure 12: CS setup/high >=350 ns、hold >= SCK半周期。
 * 従来の各1us待ちを、500ns以上かつ半SCK周期以上の共通ガードへ短縮する。
 * initでCPU/APB2クロックから切上げ計算。実際のGPIO/関数の時間はさらに加わる。
 * DWT差はunsignedで計算し周回に対応。割り込みを禁止せず長くなる方向を許容。
 * SPI/DMAレジスタ操作の順序、BSY確認、各フレームのCS区切りは維持する。 */
static void DelayCsSince(uint32_t start)
{
  while ((uint32_t)(DWT->CYCCNT - start) < cs_guard_cycles) { }
}
static void DelayCs(void) { DelayCsSince(DWT->CYCCNT); }
static void RaiseCs(void)
{
  SPI1_SS_GPIO_Port->BSRR = SPI1_SS_Pin;
  __DSB(); /* GPIO書込み完了後を基準にしてHigh時間を短く見積もらない。 */
  cs_high_cycles=DWT->CYCCNT;
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
  /* 前フレームのHigh後に行ったDMA準備/解析の時間もガードに算入する。
   * 時間が足りない場合だけ待つため、条件を緩めず重複待ちを削減できる。 */
  DelayCsSince(cs_high_cycles);
  SPI1_SS_GPIO_Port->BSRR = (uint32_t)SPI1_SS_Pin << 16U;
  __DSB();
  uint32_t cs_low_cycles=DWT->CYCCNT;
  __DMB(); /* DMAが読むRAMへの書き込みを、DMA起動より先に完了させる。 */
  SET_BIT(rx_dma->CCR, DMA_CCR_EN);
  SET_BIT(encoder_spi->Instance->CR2, SPI_CR2_RXDMAEN | SPI_CR2_ERRIE);
  DelayCsSince(cs_low_cycles); /* RX準備時間も含め、setupは500ns以上確保。 */
  SET_BIT(tx_dma->CCR, DMA_CCR_EN);
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
#ifdef ENCODER_TIMING_PROBE
    if (probe_armed) {
      if (state == REQUEST) probe_req_irq=DWT->CYCCNT;
      if (state == RESPONSE) probe_resp_irq=DWT->CYCCNT;
    }
#endif
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
    } else if (state == DECODING) {
      decode_waits++; /* 転送済みでも角度公開待ちなら次の取得を開始できない。 */
#ifdef ENCODER_TIMING_PROBE
      ProbeWait();
#endif
    }
    return;
  }
  state = REQUEST;
#ifdef ENCODER_TIMING_PROBE
  if (probe_armed) probe_received=0;
#endif
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
  /* 応答のDMAは既に完了。以降のCPU処理は転送タイムアウトと区別する。
   * 高優先度ISRに中断されても、FOCの角度鮮度監視は独立して継続する。 */
  if (state == RESPONSE) {
#ifdef ENCODER_TIMING_PROBE
    if (probe_armed) probe_decode=DWT->CYCCNT;
#endif
    state = DECODING;
  }
  uint16_t response = rx_word;
  /* BSY解除を確認済み。各16bitの間でCSをHighに戻す。 */
  DelayCs();
  RaiseCs();
  /* Highガードは次回StartFrame側で残り時間を確認する。
   * RESPONSEでは角度の解析・公開を先に進めてもCSをLowにしないため安全。 */
  if (state == REQUEST) {
    state = RESPONSE;
    StartFrame(0U); /* NOPが前フレームの要求に対応するデータを返す。 */
    return;
  }
  if (state != DECODING) return;
  uint32_t received = DWT->CYCCNT;
#ifdef ENCODER_TIMING_PROBE
  if (probe_armed) probe_received=received;
  if (probe_pending && probe.request == start_cycles) probe.received=received;
#endif
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
    sample->valid = diagnostic_ok && state == DECODING;
    /* 公開と要求受付再開を一緒に確定する。
     * 公開だけ先に済ませてTIM/ADCに中断されると、FOCは新角度を読めても
     * StartReadがDECODINGを見て次の要求を見送る。旧ISRの末尾で新転送の
     * stateを上書きしないよう、この枝は公開後すぐreturnする。 */
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    __DMB();
    published = next;
    state = IDLE;
#ifdef ENCODER_TIMING_PROBE
    if (probe_pending && probe.request == start_cycles) {
      probe.new_seq=sample->sequence;
      probe.publish=DWT->CYCCNT;
    }
#endif
    __set_PRIMASK(mask);
    return;
  }
  __DMB();
  if (state == DECODING) state = IDLE;
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
#ifdef ENCODER_TIMING_PROBE
  ProbeUse(sample);
#endif
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
static uint32_t CyclesToHundredthUs(uint32_t cycles)
{
  return (uint32_t)(((uint64_t)cycles * 100000000ULL + SystemCoreClock/2U) / SystemCoreClock);
}
static void PrintStatus(void)
{
  /* Print one line per main loop to avoid delaying the FOC watchdog. */
  unsigned line = status_line++;
  if (line == 1U) PrintSample();
  else if (line == 2U) {
    uint32_t transfer = CyclesToHundredthUs(transfer_cycles);
    uint32_t maximum = CyclesToHundredthUs(max_transfer_cycles);
    uint32_t interval = CyclesToHundredthUs(interval_cycles);
    uint32_t interval_max = CyclesToHundredthUs(max_interval_cycles);
    uint32_t launch = CyclesToHundredthUs(max_launch_cycles);
    printf("Encoder DMA: source=%s, transfers=%lu, transfer=%lu.%02lu/max %lu.%02lu us, interval=%lu.%02lu/max %lu.%02lu us, launch_max=%lu.%02lu us\r\n",
           (sample_timer != NULL && (sample_timer->Instance->CR1 & TIM_CR1_CEN)) ? "TIM1" : "main",
           (unsigned long)transfers,
           (unsigned long)(transfer/100U), (unsigned long)(transfer%100U),
           (unsigned long)(maximum/100U), (unsigned long)(maximum%100U),
           (unsigned long)(interval/100U), (unsigned long)(interval%100U),
           (unsigned long)(interval_max/100U), (unsigned long)(interval_max%100U),
           (unsigned long)(launch/100U), (unsigned long)(launch%100U));
  }
  else if (line == 3U) printf("Encoder errors: spi=%lu, parity=%lu, sensor=%lu, timeout=%lu, busy_ticks=%lu, decode_waits=%lu, diag=0x%04X, errfl=0x%04X\r\n",
         (unsigned long)spi_errors, (unsigned long)parity_errors, (unsigned long)sensor_errors,
         (unsigned long)timeouts, (unsigned long)missed, (unsigned long)decode_waits, (unsigned int)diagnostic, (unsigned int)error_flags);
  else if (line == 4U) printf("Angle display: elec_uncal is before direction/offset correction. FOC uses calibration; check 'cal status' for validity and Flash save status.\r\n");
  else printf("Encoder first fault: %s, state=%lu, elapsed=%lu ns, SR=0x%08lX, RXleft=%lu, TXleft=%lu, DMA=0x%08lX\r\n",
      first_fault ? first_fault : "none", (unsigned long)fault_state,
      (unsigned long)(((uint64_t)fault_cycles * 1000000000ULL) / SystemCoreClock),
      (unsigned long)fault_sr,(unsigned long)fault_rx_left,(unsigned long)fault_tx_left,
      (unsigned long)fault_dma_flags);
  if (line >= 5U) status_line = 0U;
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
#ifdef ENCODER_TIMING_PROBE
  if (Equals(command, "angle timing arm")) {
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    probe_armed=false; probe_pending=false; probe_ready=false;
    probe=(TimingProbe){0};
    probe_req_irq=probe_resp_irq=probe_decode=probe_adc=0;
    probe_received=0;
    probe_use_time=probe_use_seq=probe_use_request=0;
    probe_armed=true;
    __set_PRIMASK(mask);
    printf("TIMING armed\r\n"); return true;
  }
  if (Equals(command, "angle timing dump")) { ProbeDump(); return true; }
#endif
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
void AS5047P_Task(void)
{
  if (encoder_spi == NULL || state == OFF) {
    if (status_line) PrintStatus();
    return;
  }
  if ((state == REQUEST || state == RESPONSE) && Expired()) {
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    if ((state == REQUEST || state == RESPONSE) && Expired()) { RecordFault("main transfer timeout"); timeouts++; Fail(); }
    __set_PRIMASK(mask);
  }
  if (state == FAILED && MotorControl_IsStopped()) {
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
    RaiseCs();
    DelayCs();
    if (result == HAL_OK) __HAL_SPI_ENABLE(encoder_spi);
    state = result == HAL_OK ? IDLE : OFF;
    if (result != HAL_OK) printf("Encoder DMA recovery failed; reset required\r\n");
  }
  uint32_t now = HAL_GetTick();
  /* TIM1停止中の手回し観測。タイマ/PWMはここから開始しない。 */
  if ((sample_timer->Instance->CR1 & TIM_CR1_CEN) == 0U && now != fallback_ms) {
    fallback_ms = now; StartRead();
  }
  if (status_line) { PrintStatus(); return; }
  if (streaming && (uint32_t)(now - report_ms) >= 100U) { report_ms = now; PrintSample(); }
}
void AS5047P_Init(SPI_HandleTypeDef *spi, TIM_HandleTypeDef *timer)
{
  state = OFF;
  if (spi == NULL || timer == NULL || spi->hdmarx == NULL || spi->hdmatx == NULL) {
    printf("Encoder DMA configuration missing\r\n"); return;
  }
  /* 16bit・通常DMA専用。CubeMX設定が変わったら黙って不正転送せず停止する。 */
  if (spi->Instance != SPI1 || spi->Init.DataSize != SPI_DATASIZE_16BIT || spi->Init.Mode != SPI_MODE_MASTER ||
      spi->Init.Direction != SPI_DIRECTION_2LINES || spi->Init.NSS != SPI_NSS_SOFT ||
      spi->Init.CRCCalculation != SPI_CRCCALCULATION_DISABLE ||
      timer->Instance != TIM1 ||
      spi->hdmarx->Init.Request != DMA_REQUEST_SPI1_RX ||
      spi->hdmatx->Init.Request != DMA_REQUEST_SPI1_TX ||
      ((SPI1_SS_GPIO_Port->MODER >> (15U * 2U)) & 3U) != 1U ||
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
  /* このドライバはSPI1/APB2専用。動作中のクロック変更には再Initが必要。
   * BR値は2,4,...256分周。遅いSPI設定でもholdを短くし過ぎない。 */
  uint32_t spi_divider=2U << (spi->Init.BaudRatePrescaler >> SPI_CR1_BR_Pos);
  uint32_t pclk=HAL_RCC_GetPCLK2Freq();
  cs_guard_cycles=(SystemCoreClock+1999999U)/2000000U;
  uint32_t half_sck=(uint32_t)(((uint64_t)SystemCoreClock*spi_divider+2ULL*pclk-1U)/(2ULL*pclk));
  if (cs_guard_cycles<half_sck) cs_guard_cycles=half_sck;
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
  RaiseCs();
  HAL_Delay(10U);
  state = IDLE;
  uint32_t began = HAL_GetTick();
  while (latest.sequence == 0U && (uint32_t)(HAL_GetTick() - began) < 30U) AS5047P_Task();
  status_line = 1U;
  while (status_line) PrintStatus();
}

/* Flash書き込み前のmain専用。PWM停止中にだけ使用する。 */
void AS5047P_Pause(void)
{
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  if (!fast_owned) { __set_PRIMASK(mask); return; }
  state = OFF; StopDma();
  (void)WaitIdle();
  RaiseCs();
  latest.valid = false; diagnostic_ok = false;
  __set_PRIMASK(mask);
}
void AS5047P_Resume(void)
{
  /* 途中の応答は破棄し、既存のmain復旧処理でFIFOを排出する。 */
  if (fast_owned) state = FAILED;
}
