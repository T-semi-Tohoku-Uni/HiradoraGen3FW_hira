#ifndef AS5047P_H
#define AS5047P_H
#include "stm32g4xx_hal.h"
#include <stdbool.h>

/* electrical_radはdirection/offset未校正。FOCはmechanical_radを校正して使う。 */
typedef struct {
  uint16_t raw;
  float mechanical_rad, electrical_rad;
  /* request: 応答に対応する前フレームのTIM8送信予定時刻（保守的な基点）。
   * received: 応答のISR取得時刻。どちらもセンサー内部の測定時刻ではない。 */
  uint32_t request_cycles, received_cycles, updated_ms, sequence;
  bool valid;
} AS5047P_Sample;
void AS5047P_Init(SPI_HandleTypeDef *spi, TIM_HandleTypeDef *timer);
void AS5047P_Task(void);
void AS5047P_Watchdog(void); /* main/ISR共用。通信開始はTIM8が自動で行う。 */
bool AS5047P_GetSample(AS5047P_Sample *sample);
/* Read-only phase query; caller masks IRQs for an atomic start decision. */
bool AS5047P_GetTimerPhase(uint32_t *count);
bool AS5047P_ProcessCommand(const char *command);
/* 専用DMAが所有するIRQならtrueを返す。HAL IRQとの二重処理を防ぐ。 */
bool AS5047P_DMA_IRQHandler(DMA_HandleTypeDef *dma);
bool AS5047P_SPI_IRQHandler(SPI_HandleTypeDef *spi);
void AS5047P_Pause(void);
void AS5047P_Resume(void);
#endif
