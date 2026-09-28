#ifndef CONSOLE_H
#define CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef CONSOLE_TX_BUFFER_SIZE
/* printfの文字列を一時的に保存する送信リングバッファのサイズ */
#define CONSOLE_TX_BUFFER_SIZE 512U
#endif

#ifndef CONSOLE_RX_LINE_SIZE
/* 改行文字と終端の'\0'を除き、最大63文字まで受信できます。 */
#define CONSOLE_RX_LINE_SIZE 64U
#endif

/**
 * @brief コンソール出力に使用するUARTを登録します。
 * @param huart CubeMXが生成したUARTハンドルのアドレス
 *
 * UARTとDMAの初期化が完了した後に、一度だけ呼び出してください。
 * 例: Console_Init(&huart1);
 */
void Console_Init(UART_HandleTypeDef *huart);

/**
 * @brief 指定されたデータをUARTのDMA送信待ちバッファへ追加します。
 * @param data 送信するデータの先頭アドレス
 * @param length 送信するバイト数
 * @return バッファへ追加できたバイト数
 *
 * main関数から呼んだ場合、バッファが満杯なら空きができるまで待ちます。
 * 割り込み処理から呼んだ場合は、満杯になった時点で処理を終了します。
 */
size_t Console_Write(const void *data, size_t length);

/* Main context only. Borrow a complete text block until callback (ISR).
 * Returns HAL_BUSY without waiting if text or another block is pending.
 * The caller must keep data immutable until done(success) is called.
 * Text queued during the transfer is sent after the complete block. */
HAL_StatusTypeDef Console_TryTransmitBlock(const void *data, uint16_t length,
                                          void (*done)(bool success));

/**
 * @brief 受信した文字列をfloatへ変換し、指定された変数へ保存します。
 * @param received_value 正常に受信した値の保存先
 *
 * mainループから繰り返し呼び出してください。新しい行を受信していなければ
 * 何も行いません。文字列取得、数値変換、入力検証、結果表示は、この関数の
 * 中で行います。
 */
void Console_Process(float *received_value);

/**
 * @brief バッファ内の全データが送信されるまで待ちます。
 * @param timeout_ms 最大待ち時間（ミリ秒）
 * @return 送信完了ならHAL_OK、時間切れならHAL_TIMEOUT
 */
HAL_StatusTypeDef Console_Flush(uint32_t timeout_ms);

/**
 * @brief Copy one completed UART input line into the caller's buffer.
 * @return true when a line was copied, otherwise false.
 */
bool Console_ReadLine(char *destination, size_t destination_size);
bool Console_ProcessCommand(const char *command);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_H */
