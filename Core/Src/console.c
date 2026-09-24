#include "console.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if (CONSOLE_TX_BUFFER_SIZE < 2U) || (CONSOLE_TX_BUFFER_SIZE > 65535U)
#error "CONSOLE_TX_BUFFER_SIZE must be between 2 and 65535"
#endif

#if (CONSOLE_RX_LINE_SIZE < 2U) || (CONSOLE_RX_LINE_SIZE > 65535U)
#error "CONSOLE_RX_LINE_SIZE must be between 2 and 65535"
#endif

/* Console_Init()で登録される、コンソール出力用のUARTです。 */
static UART_HandleTypeDef *console_uart;

/*
 * printfから受け取った文字列を一時保存するリングバッファです。
 *
 * tx_write_index : 次のデータを書き込む位置
 * tx_read_index  : 次のDMA送信を開始する位置
 *
 * 各インデックスはバッファ末尾まで進むと0へ戻ります。
 * 空と満杯を区別するため、常に1バイト分を未使用にします。
 */
static uint8_t tx_buffer[CONSOLE_TX_BUFFER_SIZE];
static volatile uint16_t tx_write_index;
static volatile uint16_t tx_read_index;

/*
 * 以下の変数はmain側とDMA完了割り込み側の両方から参照します。
 * volatileを付け、コンパイラによる不要な読み書きの省略を防ぎます。
 */
static volatile uint16_t current_dma_length;
static volatile uint8_t is_dma_transmitting;
static void (*block_done)(bool success);

/*
 * UART受信用の変数です。
 *
 * rx_blocks           : HALが受信する交互バッファ
 * rx_line_buffer      : 改行までの文字列を保存する場所
 * rx_line_length      : 現在保存されている文字数
 * rx_head/rx_tail     : 完成行キューの書込み/読出し位置
 * is_rx_line_overflow : 受信文字列が長すぎたことを示すフラグ
 */
/* HALのReceiveToIdleで複数バイトを受ける。1文字ごとの再設定を避ける。
 * コールバックで次のバッファへ切り替え、完了側を解析する間も受信を継続。 */
static uint8_t rx_blocks[2][64];
static uint8_t rx_slot;
static void Console_StartReceive(void);
static volatile uint32_t rx_errors;
static volatile uint32_t rx_overruns;
static char rx_line_buffer[CONSOLE_RX_LINE_SIZE];
static volatile uint16_t rx_line_length;
/* ISRが完成行を追加しmainが取り出す。処理中も次の行を受信できる。
 * 1スロットを空けるSPSC方式で8行保持。満杯時は行全体を捨て、件数を公開。 */
#define RX_QUEUE_SLOTS 9U
static char rx_lines[RX_QUEUE_SLOTS][CONSOLE_RX_LINE_SIZE];
static uint16_t rx_lengths[RX_QUEUE_SLOTS];
static volatile uint8_t rx_head, rx_tail;
static volatile uint32_t rx_queue_drops, rx_long_lines, rx_error_flags;
static volatile uint8_t is_rx_line_overflow;

/**
 * @brief バッファに未送信データがあればDMA送信を開始します。
 *
 * すでにDMA送信中の場合は何もせずに戻ります。
 * リングバッファの末尾をまたぐデータは、前半と後半の2回に分けて
 * DMA送信します。
 */
static void Console_StartTransmit(void)
{
  uint16_t transmit_length;
  uint32_t interrupt_state;

  /*
   * DMA完了割り込みと同時に変数を書き換えないよう、一時的に
   * 割り込みを禁止します。元の割り込み状態は後で復元します。
   */
  interrupt_state = __get_PRIMASK();
  __disable_irq();

  if ((console_uart == NULL) ||
      (is_dma_transmitting != 0U) ||
      (tx_write_index == tx_read_index))
  {
    if (interrupt_state == 0U)
    {
      __enable_irq();
    }
    return;
  }

  /* 現在の読み出し位置から連続して並んでいるデータ数を求めます。 */
  if (tx_write_index > tx_read_index)
  {
    transmit_length = (uint16_t)(tx_write_index - tx_read_index);
  }
  else
  {
    /* 書き込み位置が一周している場合は、まずバッファ末尾まで送ります。 */
    transmit_length = (uint16_t)(CONSOLE_TX_BUFFER_SIZE - tx_read_index);
  }

  current_dma_length = transmit_length;
  is_dma_transmitting = 1U;

  if (interrupt_state == 0U)
  {
    __enable_irq();
  }

  /* DMAへ、送信元アドレスと送信バイト数を渡します。 */
  if (HAL_UART_Transmit_DMA(console_uart,
                            &tx_buffer[tx_read_index],
                            transmit_length) != HAL_OK)
  {
    /* 開始できなかった場合、次回再試行できるよう送信中状態を解除します。 */
    interrupt_state = __get_PRIMASK();
    __disable_irq();
    is_dma_transmitting = 0U;
    current_dma_length = 0U;
    if (interrupt_state == 0U)
    {
      __enable_irq();
    }
  }
}

void Console_Init(UART_HandleTypeDef *huart)
{
  uint32_t interrupt_state;

  /* 初期化の途中でDMA完了割り込みが入らないようにします。 */
  interrupt_state = __get_PRIMASK();
  __disable_irq();

  console_uart = huart;
  block_done = NULL;
  tx_write_index = 0U;
  tx_read_index = 0U;
  current_dma_length = 0U;
  is_dma_transmitting = 0U;
  rx_line_length = 0U;
  rx_head = rx_tail = 0U;
  rx_queue_drops = rx_long_lines = rx_error_flags = 0U;
  is_rx_line_overflow = 0U;

  if (interrupt_state == 0U)
  {
    __enable_irq();
  }

  /* FIFOを使い、最大64バイトまたはIDLEまでをHALで受信する。 */
  if (console_uart != NULL)
  {
    Console_StartReceive();
  }
}

HAL_StatusTypeDef Console_TryTransmitBlock(const void *data, uint16_t length,
                                          void (*done)(bool success))
{
  if (console_uart == NULL || data == NULL || length == 0U || done == NULL) {
    return HAL_ERROR;
  }
  Console_StartTransmit();
  const uint32_t mask = __get_PRIMASK();
  __disable_irq();
  if (is_dma_transmitting || tx_read_index != tx_write_index) {
    __set_PRIMASK(mask);
    return HAL_BUSY;
  }
  is_dma_transmitting = 1U;
  block_done = done;
  /* DMA起動中もRXを許可。921600bpsでは約10.85usごとに次の文字が来る。 */
  __set_PRIMASK(mask);
  const HAL_StatusTypeDef status = HAL_UART_Transmit_DMA(
    console_uart, (const uint8_t *)data, length);
  if (status != HAL_OK) {
    block_done = NULL;
    is_dma_transmitting = 0U;
  }
  __set_PRIMASK(mask);
  return status;
}

size_t Console_Write(const void *data, size_t length)
{
  const uint8_t *source_data = (const uint8_t *)data;
  size_t written_length = 0U;

  /* NULLポインタや、Console_Init前の呼び出しでは送信しません。 */
  if ((source_data == NULL) || (console_uart == NULL))
  {
    return 0U;
  }

  /* 受け取ったデータを1バイトずつリングバッファへ追加します。 */
  while (written_length < length)
  {
    uint16_t next_write_index;
    uint32_t interrupt_state;

    /* DMA完了割り込み側もインデックスを操作するため、ここでは排他します。 */
    interrupt_state = __get_PRIMASK();
    __disable_irq();

    next_write_index = (uint16_t)(tx_write_index + 1U);
    if (next_write_index >= CONSOLE_TX_BUFFER_SIZE)
    {
      next_write_index = 0U;
    }

    /* 次の書き込み位置が読み出し位置と同じなら、バッファは満杯です。 */
    if (next_write_index != tx_read_index)
    {
      tx_buffer[tx_write_index] = source_data[written_length];
      tx_write_index = next_write_index;
      written_length++;
    }

    if (interrupt_state == 0U)
    {
      __enable_irq();
    }

    /* DMAが停止中なら、ここで送信を開始します。 */
    Console_StartTransmit();

    /*
     * 割り込み処理内では、バッファの空きを待ち続けることができません。
     * 満杯の場合は、ここまでに追加できたバイト数を返して終了します。
     */
    if ((next_write_index == tx_read_index) && (__get_IPSR() != 0U))
    {
      break;
    }
  }

  return written_length;
}

/**
 * @brief 改行まで受信した文字列を取り出します。
 * @return 1行受信済みならtrue、まだ受信中ならfalse
 *
 * 末尾のCR/LFはコピーしません。
 */
bool Console_ReadLine(char *destination, size_t destination_size)
{
  if (!destination || !destination_size) return false;
  uint8_t tail=rx_tail;
  if (tail==rx_head) return false;
  __DMB();
  /* tailを進めるまではISRはこのスロットを上書きしない。コピー中のIRQ禁止は不要。 */
  uint16_t length=rx_lengths[tail];
  if (length<destination_size) {
    memcpy(destination,rx_lines[tail],length);
    destination[length]='\0';
  } else destination[0]='\0';
  __DMB();
  rx_tail=(tail+1U==RX_QUEUE_SLOTS) ? 0U : tail+1U;
  return true;
}

void Console_Process(float *received_value)
{
  char received_line[CONSOLE_RX_LINE_SIZE];
  char *parse_end;
  float parsed_value;

  if (received_value == NULL)
  {
    return;
  }

  /* まだ改行まで受信していない場合は、すぐにmainループへ戻ります。 */
  if (!Console_ReadLine(received_line, sizeof(received_line)))
  {
    return;
  }

  /* 受信文字列をfloatへ変換します。 */
  errno = 0;
  parsed_value = strtof(received_line, &parse_end);

  /*
   * 次の条件をすべて満たす場合だけ、呼び出し元の変数へ反映します。
   *
   * ・1文字以上を数値へ変換できた
   * ・文字列の最後まで数値として解釈できた
   * ・floatの表現範囲を超えていない
   * ・NaNや無限大ではない
   */
  if ((parse_end != received_line) &&
      (*parse_end == '\0') &&
      (errno != ERANGE) &&
      isfinite(parsed_value))
  {
    *received_value = parsed_value;
    printf("received_value = %.3f\r\n", *received_value);
  }
  else
  {
    printf("Invalid value: %s\r\n", received_line);
  }

  printf("Input value:\r\n");
}

HAL_StatusTypeDef Console_Flush(uint32_t timeout_ms)
{
  uint32_t start_tick = HAL_GetTick();

  if (console_uart == NULL)
  {
    return HAL_ERROR;
  }

  /* バッファが空になり、DMA送信も終了するまで待ちます。 */
  while ((tx_write_index != tx_read_index) || (is_dma_transmitting != 0U))
  {
    /* 何らかの理由でDMAが始まっていなければ、ここで再試行します。 */
    Console_StartTransmit();

    if ((HAL_GetTick() - start_tick) >= timeout_ms)
    {
      return HAL_TIMEOUT;
    }
  }

  return HAL_OK;
}

int _write(int file, char *data, int length)
{
  size_t written_length;

  /* fileはstdout/stderrの識別子ですが、この実装では同じUARTへ送ります。 */
  (void)file;

  if ((data == NULL) || (length <= 0))
  {
    return 0;
  }

  /* printfは最終的に_writeを呼ぶため、ここからDMA送信へ接続します。 */
  written_length = Console_Write(data, (size_t)length);
  return (int)written_length;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  uint32_t interrupt_state;

  /* コンソール以外のUARTで発生した完了通知は処理しません。 */
  if ((huart != console_uart) || (is_dma_transmitting == 0U))
  {
    return;
  }

  if (block_done != NULL) {
    void (*done)(bool) = block_done;
    block_done = NULL;
    is_dma_transmitting = 0U;
    done(true);
    Console_StartTransmit();
    return;
  }

  /*
   * この関数は、UARTのDMA送信が完了したときにHALから呼ばれます。
   * 送信済みのバイト数だけ読み出し位置を進めます。
   */
  interrupt_state = __get_PRIMASK();
  __disable_irq();

  tx_read_index = (uint16_t)(tx_read_index + current_dma_length);
  if (tx_read_index >= CONSOLE_TX_BUFFER_SIZE)
  {
    tx_read_index = (uint16_t)(tx_read_index - CONSOLE_TX_BUFFER_SIZE);
  }

  current_dma_length = 0U;
  is_dma_transmitting = 0U;

  if (interrupt_state == 0U)
  {
    __enable_irq();
  }

  /* バッファに続きがあれば、次のDMA送信を開始します。 */
  Console_StartTransmit();
}

static void Console_StartReceive(void)
{
  if (HAL_UARTEx_ReceiveToIdle_IT(console_uart,rx_blocks[rx_slot],sizeof(rx_blocks[0]))!=HAL_OK)
    rx_errors++;
}
static void Console_ReceiveByte(uint8_t received_byte)
{
  if (received_byte=='\r' || received_byte=='\n') {
    if (rx_line_length || is_rx_line_overflow) {
      uint8_t head=rx_head;
      uint8_t next=(head+1U==RX_QUEUE_SLOTS) ? 0U : head+1U;
      if (next==rx_tail) rx_queue_drops++;
      else {
        uint16_t length=is_rx_line_overflow ? 0U : rx_line_length;
        memcpy(rx_lines[head],rx_line_buffer,length);
        rx_lengths[head]=length;
        __DMB();
        rx_head=next;
      }
      rx_line_length=0U; is_rx_line_overflow=0U;
    }
  } else if (!is_rx_line_overflow) {
    if (rx_line_length<CONSOLE_RX_LINE_SIZE-1U)
      rx_line_buffer[rx_line_length++]=(char)received_byte;
    else { is_rx_line_overflow=1U; rx_long_lines++; }
  }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart,uint16_t size)
{
  if(huart!=console_uart) return;
  /* HALはエラーIRQでも受信完了を先に通知する場合がある。
   * ここで再受信するとErrorCodeが消えるので、異常時はErrorCallbackへ任せる。 */
  if(huart->ErrorCode!=HAL_UART_ERROR_NONE) return;
  uint8_t completed=rx_slot;
  rx_slot^=1U;
  Console_StartReceive();
  for(uint16_t i=0;i<size && i<sizeof(rx_blocks[0]);i++) Console_ReceiveByte(rx_blocks[completed][i]);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart != console_uart)
  {
    return;
  }
  rx_errors++;
  rx_error_flags |= huart->ErrorCode;
  if ((huart->ErrorCode & HAL_UART_ERROR_ORE) != 0U) rx_overruns++;

  /* HAL has stopped the transfer on a DMA error. RX framing/overrun
   * errors alone must not release a buffer still being read by TX DMA. */
  if ((huart->ErrorCode & HAL_UART_ERROR_DMA) != 0U) {
    void (*done)(bool) = block_done;
    block_done = NULL;
    is_dma_transmitting = 0U;
    current_dma_length = 0U;
    if (done != NULL) done(false);
    Console_StartTransmit();
  }

  /* 完成済みの行は保持し、エラーを含む作成中の行だけ改行まで破棄する。 */
  rx_line_length=0U;
  is_rx_line_overflow=1U;

  /* 壊れたFIFO/途中バッファを残さず、RXだけを停止・再開する。TX DMAは維持。 */
  (void)HAL_UART_AbortReceive(console_uart);
  __HAL_UART_SEND_REQ(console_uart, UART_RXDATA_FLUSH_REQUEST);
  Console_StartReceive();
}

bool Console_ProcessCommand(const char *command)
{
  if (command == NULL || strcmp(command, "serial status") != 0) return false;
  printf("Serial RX: errors=%lu, overrun=%lu, queue_drops=%lu, long_lines=%lu, flags=0x%lX\r\n",
         (unsigned long)rx_errors,(unsigned long)rx_overruns,
         (unsigned long)rx_queue_drops,(unsigned long)rx_long_lines,(unsigned long)rx_error_flags);
  return true;
}
