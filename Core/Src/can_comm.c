#include "can_comm.h"
#include "can_protocol.h"
#include "console.h"
#include "as5047p.h"
#include "can_telemetry.h"
#include "foc_voltage.h"
#include "ntc.h"
#include "can_safety.h"
#include "motor_control.h"
#include "motor_calibration.h"
#include "current_sense.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* フィードバックは1msに統一。CubeMX設定とは独立。 */
#define CAN_FEEDBACK_PERIOD_MS 1U

typedef struct {
  uint32_t rx, accepted, invalid, other, lost, tx_done, errors, error_flags;
  uint32_t last_ms, last_id;
  uint32_t last_error_lec;
  int16_t current;
  uint8_t data[8];
  bool valid, bus_off;
} CanStats;
static FDCAN_HandleTypeDef *can;
static volatile CanStats stats;
static volatile bool ready;
static bool monitor;
static uint32_t tx_ms, monitor_ms, shown_rx, monitor_skipped;
static uint32_t tx_queued, tx_skipped, tx_errors;
static uint32_t pending_buffer;
static volatile uint32_t awaiting_completion;
static char monitor_text[160];
static volatile bool monitor_busy;
static CanTelemetry telemetry;
static uint32_t telemetry_sequence, telemetry_invalid;
static bool angle_valid;
static bool iq_valid, temperature_valid;
static int16_t feedback_iq;
static uint8_t feedback_temperature;
static volatile CanSafety safety;
static volatile uint32_t fifo_empty_ms;
static volatile uint32_t stale_frames, prearm_frames;
static uint16_t arm_timestamp;
static volatile bool safety_stop;
static volatile uint32_t control_cycles;
static volatile bool foc_requested;

/* PRIMASK保持中に呼ぶ。SysTickが遅れてもDWTで期限を失効させる。 */
static void CheckDeadline(void)
{
  uint32_t before = safety.state;
  CanSafety_Tick(&safety, HAL_GetTick());
  if ((uint32_t)(DWT->CYCCNT - control_cycles) >= SystemCoreClock / 20U)
    CanSafety_Trip(&safety, CAN_TRIP_TIMEOUT);
  if (before != safety.state) safety_stop = true;
}
static bool ControlPermitted(void)
{
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  CheckDeadline();
  bool permitted = safety.state == CAN_SAFE_READY && !stats.bus_off;
  __set_PRIMASK(mask);
  return permitted;
}

static void SafetyTrip(uint32_t reason)
{
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  uint32_t before = safety.state;
  CanSafety_Trip(&safety, reason);
  if (before != safety.state) safety_stop = true;
  __set_PRIMASK(mask);
}

void CanComm_WatchdogISR(void)
{
  if (!ready) return;
  uint32_t mask = __get_PRIMASK(); __disable_irq();
  uint32_t before = safety.state;
  CheckDeadline();
  if (before == CAN_SAFE_WAIT_ZERO || before == CAN_SAFE_READY) {
    if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_15) == GPIO_PIN_RESET)
      CanSafety_Trip(&safety, CAN_TRIP_GATE);
    if (before != safety.state) safety_stop = true;
  }
  bool stop = safety_stop;
  safety_stop = false;
  __set_PRIMASK(mask);
  if (stop) {
    if (foc_requested) FocVoltage_TripISR("CAN safety stop");
    MotorControl_Stop();
  }
}

static CanStats Snapshot(void)
{
  /* 複数フィールドを同じ受信時点の組として取得する。 */
  uint32_t mask = __get_PRIMASK();
  __disable_irq();
  CanStats copy = stats;
  __set_PRIMASK(mask);
  return copy;
}

void CanComm_Init(FDCAN_HandleTypeDef *handle)
{
  can = handle;
  FDCAN_FilterTypeDef filter = {0};
  filter.IdType = FDCAN_STANDARD_ID;
  filter.FilterType = FDCAN_FILTER_DUAL;
  filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
  filter.FilterID1 = 0x200U;
  filter.FilterID2 = 0x1FFU;
  uint32_t notifications = FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_RX_FIFO0_MESSAGE_LOST |
      FDCAN_IT_TX_COMPLETE | FDCAN_IT_BUS_OFF | FDCAN_IT_ERROR_WARNING |
      FDCAN_IT_ERROR_PASSIVE | FDCAN_IT_ARB_PROTOCOL_ERROR | FDCAN_IT_DATA_PROTOCOL_ERROR;
  if (HAL_FDCAN_ConfigFilter(can, &filter) != HAL_OK ||
      HAL_FDCAN_ConfigGlobalFilter(can, FDCAN_REJECT, FDCAN_REJECT,
          FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE) != HAL_OK ||
      HAL_FDCAN_ConfigRxFifoOverwrite(can, FDCAN_RX_FIFO0, FDCAN_RX_FIFO_BLOCKING) != HAL_OK ||
      HAL_FDCAN_ConfigTimestampCounter(can, FDCAN_TIMESTAMP_PRESC_16) != HAL_OK ||
      HAL_FDCAN_EnableTimestampCounter(can, FDCAN_TIMESTAMP_INTERNAL) != HAL_OK ||
      HAL_FDCAN_ActivateNotification(can, notifications,
          FDCAN_TX_BUFFER0 | FDCAN_TX_BUFFER1 | FDCAN_TX_BUFFER2) != HAL_OK ||
      HAL_FDCAN_Start(can) != HAL_OK) {
    printf("CAN init failed; test disabled\r\n");
    return;
  }
  ready = true;
  fifo_empty_ms = HAL_GetTick();
  tx_ms = HAL_GetTick();
  printf("CAN: node=%u, 1Mbps, TX/%ums; explicit arm required\r\n", CAN_NODE_ID, CAN_FEEDBACK_PERIOD_MS);
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *handle, uint32_t flags)
{
  if (handle != can) return;
  bool lost = (flags & FDCAN_IT_RX_FIFO0_MESSAGE_LOST) != 0U;
  if (lost) { stats.lost++; stats.valid = false; SafetyTrip(CAN_TRIP_FIFO); }
  /* 1Mbps/16bit/16分周でtimestampは約1.048秒で周回。
   * FIFO空を確認できない期間が1秒に達したら周回数が不明なので破棄する。 */
  bool ambiguous = (uint32_t)(HAL_GetTick() - fifo_empty_ms) >= 1000U;
  /* G4 FIFOは3要素。割り込み内の処理量を制限し、計算・表示はmainへ渡す。
   * HALは最大64byteを書き得るため、DLC検証前のバッファも64byte確保する。 */
  for (unsigned i = 0; i < 3U && HAL_FDCAN_GetRxFifoFillLevel(can, FDCAN_RX_FIFO0); ++i) {
    FDCAN_RxHeaderTypeDef header;
    uint8_t data[64];
    if (HAL_FDCAN_GetRxMessage(can, FDCAN_RX_FIFO0, &header, data) != HAL_OK) {
      stats.errors++; stats.valid = false; SafetyTrip(CAN_TRIP_RX); break;
    }
    stats.rx++;
    if (lost || ambiguous) { stale_frames++; SafetyTrip(CAN_TRIP_STALE); continue; }
    if (header.IdType != FDCAN_STANDARD_ID || header.RxFrameType != FDCAN_DATA_FRAME ||
        header.FDFormat != FDCAN_CLASSIC_CAN || header.DataLength != FDCAN_DLC_BYTES_8) {
      stats.invalid++; continue;
    }
    stats.last_id = header.Identifier;
    memcpy((void *)stats.data, data, 8U);
    int16_t current;
    if (CanProtocol_Decode(CAN_NODE_ID, header.Identifier, data, 8U, &current)) {
      uint16_t timestamp = HAL_FDCAN_GetTimestampCounter(can);
      uint32_t age_us = (uint16_t)(timestamp - header.RxTimestamp) * 16U;
      if (age_us > 5000U) {
        stale_frames++; stats.valid = false; SafetyTrip(CAN_TRIP_STALE); continue;
      }
      stats.current = current;
      /* これはFIFO読出し時刻。制御用の鮮度判定には使用しない。 */
      stats.last_ms = HAL_GetTick();
      stats.accepted++;
      stats.valid = true;
      uint32_t mask = __get_PRIMASK(); __disable_irq();
      CheckDeadline();
      /* WAIT_ZEROは50msで失効するため、この差分比較ではtimestamp一周を跨がない。
       * アーム前に送信開始済みだったフレームを含め、古いゼロで許可しない。 */
      if (safety.state == CAN_SAFE_WAIT_ZERO &&
          ((uint16_t)(timestamp - arm_timestamp) <= (uint16_t)(timestamp - header.RxTimestamp))) {
        prearm_frames++;
      } else {
        uint32_t before = safety.state;
        if (CanSafety_Receive(&safety, current,
            HAL_GetTick() - (age_us + 999U) / 1000U, HAL_GetTick()))
          control_cycles = DWT->CYCCNT - age_us * (SystemCoreClock / 1000000U);
        if (before != CAN_SAFE_FAULT && safety.state == CAN_SAFE_FAULT) safety_stop = true;
      }
      __set_PRIMASK(mask);
    } else stats.other++;
  }
  if (HAL_FDCAN_GetRxFifoFillLevel(can, FDCAN_RX_FIFO0) == 0U) fifo_empty_ms = HAL_GetTick();
}

void HAL_FDCAN_TxBufferCompleteCallback(FDCAN_HandleTypeDef *handle, uint32_t buffers)
{
  if (handle != can) return;
  /* TXBTOは以前のバッファの完了ビットも保持する。今回未集計分だけ数える。 */
  uint32_t completed = buffers & awaiting_completion;
  awaiting_completion &= ~completed;
  for (unsigned i = 0; i < 3U; ++i) if (completed & (1U << i)) stats.tx_done++;
}
void HAL_FDCAN_ErrorStatusCallback(FDCAN_HandleTypeDef *handle, uint32_t flags)
{
  if (handle != can) return;
  stats.errors++;
  stats.error_flags |= flags;
  /* 自動復帰しない。通信試験の再開はリセットで行う。 */
  if (flags & FDCAN_IT_BUS_OFF) { stats.bus_off = true; stats.valid = false; SafetyTrip(CAN_TRIP_BUS_OFF); }
}
void HAL_FDCAN_ErrorCallback(FDCAN_HandleTypeDef *handle)
{
  if (handle == can) {
    uint32_t error = HAL_FDCAN_GetError(can);
    if (error) {
      stats.errors++; stats.error_flags |= error;
      FDCAN_ProtocolStatusTypeDef protocol;
      if (HAL_FDCAN_GetProtocolStatus(can, &protocol) == HAL_OK)
        stats.last_error_lec = protocol.LastErrorCode;
    }
    /* HALは累積ErrorCodeが非ゼロなら正常IRQでも再通知する。
     * 履歴はstatsに保存したので、HAL側の通知済みコードを消費する。 */
    can->ErrorCode = HAL_FDCAN_ERROR_NONE;
  }
}

static void PrintStatus(bool capture)
{
  /* 運転中のmain監視期限を守るため、1ループにつき1行だけ表示する。 */
  static unsigned line;
  static CanStats s;
  static FDCAN_ErrorCountersTypeDef counters;
  static FDCAN_ProtocolStatusTypeDef protocol;
  static CanSafety gate;
  if (capture) {
    s = Snapshot();
    counters = (FDCAN_ErrorCountersTypeDef){0};
    protocol = (FDCAN_ProtocolStatusTypeDef){0};
    (void)HAL_FDCAN_GetErrorCounters(can, &counters);
    (void)HAL_FDCAN_GetProtocolStatus(can, &protocol);
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    gate = safety;
    __set_PRIMASK(mask);
    line = 1U; return;
  }
  if (!line) return;
    if (line == 1U) printf("CAN test node=%u ready=%u bus_off=%u; TX=angle/rpm/%ums\r\n", CAN_NODE_ID, ready, s.bus_off, CAN_FEEDBACK_PERIOD_MS);
    else if (line == 2U) printf("CAN safety state=%lu reason=%lu raw=%ld age_ms=%lu stale=%lu prearm=%lu; PWM=arm-controlled\r\n",
        (unsigned long)gate.state, (unsigned long)gate.reason, (long)gate.command,
        (unsigned long)(HAL_GetTick()-gate.last_ms), (unsigned long)stale_frames, (unsigned long)prearm_frames);
    else if (line == 3U) printf("CAN feedback angle=%u rpm=%d valid=%u/%u invalid_skip=%lu\r\n",
        telemetry.angle, telemetry.rpm, angle_valid, telemetry.speed_valid, (unsigned long)telemetry_invalid);
    else if (line == 4U) printf("CAN feedback Iq_raw=%d valid=%u board_temp=%u C valid=%u (not motor temp)\r\n",
        feedback_iq, iq_valid, feedback_temperature, temperature_valid);
    else if (line == 5U) printf("CAN RX=%lu own=%lu other=%lu invalid=%lu lost=%lu\r\n",
        (unsigned long)s.rx, (unsigned long)s.accepted, (unsigned long)s.other,
        (unsigned long)s.invalid, (unsigned long)s.lost);
    else if (line == 6U) printf("CAN TX queued=%lu done=%lu skip=%lu fail=%lu errors=%lu flags=%08lX\r\n",
        (unsigned long)tx_queued, (unsigned long)s.tx_done, (unsigned long)tx_skipped,
        (unsigned long)tx_errors, (unsigned long)s.errors, (unsigned long)s.error_flags);
    else if (line == 7U) printf("CAN bus TEC=%lu REC=%lu LEC=%lu last_error_LEC=%lu passive=%u warning=%u\r\n",
        (unsigned long)counters.TxErrorCnt, (unsigned long)counters.RxErrorCnt,
        (unsigned long)protocol.LastErrorCode, (unsigned long)s.last_error_lec, (unsigned)protocol.ErrorPassive,
        (unsigned)protocol.Warning);
    else if (line == 8U) printf("CAN last valid=%u raw=%d mA=%ld read_age_ms=%lu monitor_skipped=%lu\r\n",
        s.valid, s.current, (long)((int32_t)s.current * 20000 / 16384),
        (unsigned long)(HAL_GetTick() - s.last_ms), (unsigned long)monitor_skipped);
  line = line == 8U ? 0U : line + 1U;
}

static void MonitorDone(bool success)
{
  (void)success;
  monitor_busy = false;
}

void CanComm_Task(void)
{
  if (!ready) return;
  if (ControlPermitted()) {
    if (!foc_requested) {
      foc_requested = true;
      if (!Foc_StartCurrent(ControlPermitted)) SafetyTrip(CAN_TRIP_FOC);
    }
    if (!Foc_IsHealthy()) SafetyTrip(CAN_TRIP_FOC);
    if (ControlPermitted()) {
      /* 最新指令の読出し〜公開の間はCAN IRQだけを止め、FOC周期は維持する。 */
      uint32_t enabled = NVIC_GetEnableIRQ(FDCAN1_IT0_IRQn);
      HAL_NVIC_DisableIRQ(FDCAN1_IT0_IRQn);
      float iq = (float)safety.command * (20.0f / 16384.0f);
      if (iq > MOTOR_CONTROL_CURRENT_REF_MAX_A) iq = MOTOR_CONTROL_CURRENT_REF_MAX_A;
      if (iq < -MOTOR_CONTROL_CURRENT_REF_MAX_A) iq = -MOTOR_CONTROL_CURRENT_REF_MAX_A;
      if (!Foc_SetCurrentTarget(0.0f, iq)) SafetyTrip(CAN_TRIP_FOC);
      if (enabled) HAL_NVIC_EnableIRQ(FDCAN1_IT0_IRQn);
    }
  }
  CanComm_WatchdogISR();
  PrintStatus(false);
  uint32_t now = HAL_GetTick();
  /* FIFOが空だった時刻のみ更新し、未処理フレームの古さを隠さない。 */
  uint32_t irq_mask = __get_PRIMASK(); __disable_irq();
  if (HAL_FDCAN_GetRxFifoFillLevel(can, FDCAN_RX_FIFO0) == 0U) fifo_empty_ms = now;
  __set_PRIMASK(irq_mask);
  AS5047P_Sample sample;
  angle_valid = AS5047P_GetSample(&sample);
  if (!angle_valid) {
    telemetry = (CanTelemetry){0};
    telemetry_sequence = 0;
  } else if (sample.sequence != telemetry_sequence) {
    CanTelemetry_Update(&telemetry, sample.raw, sample.request_cycles, SystemCoreClock);
    telemetry_sequence = sample.sequence;
  }
  if ((uint32_t)(now - tx_ms) >= CAN_FEEDBACK_PERIOD_MS) {
    tx_ms = now;
    float iq = 0, temperature = 0;
    iq_valid = FocVoltage_GetIqSample(&iq);
    temperature_valid = NTC_GetTemperature(&temperature);
    /* 停止中Iq・無効温度は0を代入し、UARTで無効と区別する。
     * 予約byteには独自フラグを埋めない。温度は基板NTCの代用値。 */
    float raw_iq = iq_valid ? iq * (16384.0f / 20.0f) : 0.0f;
    if (raw_iq > 32767.0f) raw_iq = 32767.0f;
    if (raw_iq < -32768.0f) raw_iq = -32768.0f;
    feedback_iq = (int16_t)raw_iq;
    feedback_temperature = temperature_valid && temperature > 0 ? (uint8_t)temperature : 0U;
    /* 未送信フレームを積み重ねない。取消し完了も待たず次周期へ進む。 */
    if (pending_buffer && HAL_FDCAN_IsTxBufferMessagePending(can, pending_buffer)) {
      if (HAL_FDCAN_AbortTxRequest(can, pending_buffer) != HAL_OK) tx_errors++;
      tx_skipped++;
    } else if (!angle_valid || !telemetry.speed_valid) {
      /* C620互換フレームにvalid bitがないため、無効値を実測値として送らない。 */
      telemetry_invalid++; tx_skipped++;
    } else if (stats.bus_off || !HAL_FDCAN_GetTxFifoFreeLevel(can)) tx_skipped++;
    else {
      FDCAN_TxHeaderTypeDef header = {0};
      uint8_t data[8];
      (void)CanProtocol_Pack(CAN_NODE_ID, telemetry.angle, telemetry.rpm, feedback_iq,
          feedback_temperature, &header.Identifier, data);
      header.IdType = FDCAN_STANDARD_ID;
      header.TxFrameType = FDCAN_DATA_FRAME;
      header.DataLength = FDCAN_DLC_BYTES_8;
      header.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
      header.BitRateSwitch = FDCAN_BRS_OFF;
      header.FDFormat = FDCAN_CLASSIC_CAN;
      header.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
      /* 要求発行と未集計マスク公開をCAN IRQに対して不可分にする。
       * ADC/TIM/SPIを止めないため、全割り込み禁止にはしない。 */
      uint32_t irq_enabled = NVIC_GetEnableIRQ(FDCAN1_IT0_IRQn);
      HAL_NVIC_DisableIRQ(FDCAN1_IT0_IRQn);
      if (HAL_FDCAN_AddMessageToTxFifoQ(can, &header, data) == HAL_OK) {
        tx_queued++;
        pending_buffer = HAL_FDCAN_GetLatestTxFifoQRequestBuffer(can);
        awaiting_completion = pending_buffer;
      } else tx_errors++;
      if (irq_enabled) HAL_NVIC_EnableIRQ(FDCAN1_IT0_IRQn);
    }
  }
  if (!monitor || monitor_busy || (uint32_t)(now - monitor_ms) < 100U) return;
  monitor_ms = now;
  CanStats copy = Snapshot();
  if (copy.rx == shown_rx) return;
  int length = snprintf(monitor_text, sizeof(monitor_text),
      "CAN RX id=%03lX data=%02X %02X %02X %02X %02X %02X %02X %02X raw=%d\r\n",
      (unsigned long)copy.last_id, copy.data[0], copy.data[1], copy.data[2], copy.data[3],
      copy.data[4], copy.data[5], copy.data[6], copy.data[7], copy.current);
  monitor_busy = true;
  if (length > 0 && length < (int)sizeof(monitor_text) &&
      Console_TryTransmitBlock(monitor_text, (uint16_t)length, MonitorDone) == HAL_OK) {
    monitor_skipped += copy.rx - shown_rx - 1U;
    shown_rx = copy.rx;
  } else monitor_busy = false;
}

bool CanComm_ProcessCommand(const char *command)
{
  while (isspace((unsigned char)*command)) command++;
  char normalized[64];
  size_t length = strlen(command);
  while (length && isspace((unsigned char)command[length - 1U])) length--;
  if (length >= sizeof(normalized)) return true;
  for (size_t i = 0; i < length; ++i) normalized[i] = (char)tolower((unsigned char)command[i]);
  normalized[length] = 0; command = normalized;
  if (strncmp(command, "can", 3) || (command[3] && !isspace((unsigned char)command[3]))) {
    if (safety.state == CAN_SAFE_DISARMED) return false;
    if (!strcmp(command, "stop") || !strcmp(command, "foc stop")) {
      uint32_t mask = __get_PRIMASK(); __disable_irq();
      CanSafety_Disarm(&safety); safety_stop = false;
      if (foc_requested) FocVoltage_TripISR("CAN user stop");
      foc_requested = false;
      __set_PRIMASK(mask);
      MotorControl_Stop();
      return false;
    }
    /* CAN所有中は読み取り専用コマンドだけ通す。設定/校正/直接PWMを拒否。 */
    static const char *const readonly[] = {"status", "foc status", "angle status", "serial status",
                                          "cal status", "adc status", "vm", "fault"};
    for (unsigned i = 0; i < sizeof(readonly)/sizeof(readonly[0]); ++i)
      if (!strcmp(command, readonly[i])) return false;
    printf("UART control blocked: CAN owns control; use can disarm or stop\r\n");
    return true;
  }
  command += 3;
  while (isspace((unsigned char)*command)) command++;
  char arg[32];
  size_t n = strlen(command);
  while (n && isspace((unsigned char)command[n - 1U])) n--;
  if (n >= sizeof(arg)) n = sizeof(arg) - 1U;
  memcpy(arg, command, n); arg[n] = 0;
  if (!strcmp(arg, "monitor on")) { monitor = true; shown_rx = Snapshot().rx; printf("CAN monitor on (max 10Hz)\r\n"); }
  else if (!strcmp(arg, "monitor off")) { monitor = false; printf("CAN monitor off\r\n"); }
  else if (!strcmp(arg, "arm")) {
    if (safety.state == CAN_SAFE_WAIT_ZERO || safety.state == CAN_SAFE_READY) {
      printf("CAN already armed; deadline unchanged\r\n"); return true;
    }
    if (!ready || stats.bus_off || !MotorControl_IsStopped() || FocVoltage_IsActive() ||
        MotorCalibration_IsActive() || CurrentSense_IsBusy() ||
        HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_15) == GPIO_PIN_RESET) {
      printf("CAN arm rejected: stop PWM/FOC/cal/ADC, check CAN/nFAULT\r\n");
      return true;
    }
    uint32_t enabled = NVIC_GetEnableIRQ(FDCAN1_IT0_IRQn);
    SET_BIT(CoreDebug->DEMCR, CoreDebug_DEMCR_TRCENA_Msk);
    SET_BIT(DWT->CTRL, DWT_CTRL_CYCCNTENA_Msk);
    HAL_NVIC_DisableIRQ(FDCAN1_IT0_IRQn);
    /* アーム以前にFIFOへ入ったフレームは採用しない。HAL呼出し中もFOC IRQは有効。 */
    bool drained = true;
    for (unsigned i = 0; i < 3U && HAL_FDCAN_GetRxFifoFillLevel(can, FDCAN_RX_FIFO0); ++i) {
      FDCAN_RxHeaderTypeDef header; uint8_t data[64];
      if (HAL_FDCAN_GetRxMessage(can, FDCAN_RX_FIFO0, &header, data) != HAL_OK) { drained = false; break; }
      prearm_frames++;
    }
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    bool armed = drained && HAL_FDCAN_GetRxFifoFillLevel(can, FDCAN_RX_FIFO0) == 0U &&
                 CanSafety_Arm(&safety, HAL_GetTick());
    if (armed) {
      arm_timestamp = HAL_FDCAN_GetTimestampCounter(can);
      fifo_empty_ms = HAL_GetTick(); safety_stop = false; stats.valid = false;
      control_cycles = DWT->CYCCNT; foc_requested = false;
    }
    __set_PRIMASK(mask);
    if (enabled) HAL_NVIC_EnableIRQ(FDCAN1_IT0_IRQn);
    printf(armed ? "CAN armed: NEW zero within 50ms starts PWM\r\n" :
                   "CAN arm rejected: already armed or RX busy\r\n");
  }
  else if (!strcmp(arg, "disarm")) {
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    bool owned = safety.state != CAN_SAFE_DISARMED;
    CanSafety_Disarm(&safety); safety_stop = false;
    if (owned && foc_requested) FocVoltage_TripISR("CAN disarm");
    foc_requested = false;
    __set_PRIMASK(mask);
    if (owned) MotorControl_Stop();
    printf("CAN disarmed\r\n");
  }
  else if (!strcmp(arg, "status") || !*arg) {
    PrintStatus(true);
  } else printf("Usage: can status|arm|disarm|monitor on|monitor off\r\n");
  return true;
}
