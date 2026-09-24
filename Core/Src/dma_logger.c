#include "dma_logger.h"
#include "console.h"
#include <stdio.h>
#include <string.h>

typedef struct {
  uint32_t sequence;
  uint16_t raw[4];
  uint16_t sector;
} Sample;

typedef struct {
  Sample samples[DMA_LOGGER_BLOCK_SAMPLES];
  uint16_t count;
} Block;

enum { FREE, FILLING, READY };
static Block blocks[2];
static volatile uint8_t states[2];
static uint8_t producer;
static uint8_t consumer;
static uint16_t read_index;
static uint32_t divider;
static uint32_t decimation_value;
static volatile uint32_t overruns;
static volatile bool running;
static volatile bool transmitting;
static float zero_offsets[4];
static float scale;
static uint32_t sample_period_ns;
static uint32_t last_sequence;
static uint64_t elapsed_samples;
static uint32_t flush_tick;
/* One immutable DMA staging buffer; raw blocks remain small. */
static char tx_text[320];
static uint16_t tx_length;

void DmaLogger_Start(uint32_t decimation, uint32_t period_ns,
                     const float offsets[4], float amps_per_count)
{
  /* Caller has stopped the ADC producer and drained all DMA data. */
  producer = consumer = 0U;
  read_index = tx_length = 0U;
  divider = overruns = last_sequence = 0U;
  elapsed_samples = 0U;
  transmitting = false;
  decimation_value = decimation == 0U ? 1U : decimation;
  sample_period_ns = period_ns;
  memcpy(zero_offsets, offsets, sizeof(zero_offsets));
  scale = amps_per_count;
  for (unsigned int i = 0U; i < 2U; i++) {
    blocks[i].count = 0U;
    states[i] = FREE;
  }
  flush_tick = HAL_GetTick();
  running = true;
}

static void Seal(void)
{
  __DMB();
  states[producer] = READY;
  producer ^= 1U;
}

void DmaLogger_Push(uint32_t sequence, uint16_t u1, uint16_t v,
                    uint16_t u2, uint16_t w, uint8_t sector)
{
  if (!running) return;
  if (divider != 0U) { divider--; return; }
  divider = decimation_value - 1U;
  if (states[producer] == FREE) {
    blocks[producer].count = 0U;
    states[producer] = FILLING;
  }
  if (states[producer] != FILLING) { overruns++; return; }
  Block *block = &blocks[producer];
  Sample *sample = &block->samples[block->count];
  sample->sequence = sequence;
  sample->raw[0] = u1;
  sample->raw[1] = v;
  sample->raw[2] = u2;
  sample->raw[3] = w;
  sample->sector = sector;
  if (++block->count == DMA_LOGGER_BLOCK_SAMPLES) Seal();
}

void DmaLogger_Stop(void)
{
  /* Main context, only after disabling the ADC producer. */
  running = false;
  if (states[producer] == FILLING) Seal();
}

static void Transmitted(bool success)
{
  const uint32_t mask = __get_PRIMASK();
  __disable_irq();
  if (!success) overruns++;
  if (++read_index == blocks[consumer].count) {
    read_index = 0U;
    __DMB();
    states[consumer] = FREE;
    consumer ^= 1U;
  }
  tx_length = 0U;
  transmitting = false;
  __set_PRIMASK(mask);
}

void DmaLogger_Task(void)
{
  /* Seal a short block every 20 ms rather than waiting 64 sample periods.
   * Only ownership changes with interrupts masked; formatting is outside. */
  const uint32_t now = HAL_GetTick();
  if ((uint32_t)(now - flush_tick) >= 20U) {
    const uint32_t mask = __get_PRIMASK();
    __disable_irq();
    if (states[producer] == FILLING) Seal();
    __set_PRIMASK(mask);
    flush_tick = now;
  }
  if (transmitting || states[consumer] != READY) return;
  __DMB();
  if (tx_length == 0U) {
    const Sample *sample = &blocks[consumer].samples[read_index];
    elapsed_samples += (uint32_t)(sample->sequence - last_sequence);
    last_sequence = sample->sequence;
    const uint64_t time_us = elapsed_samples * sample_period_ns / 1000U;
    const unsigned long ms = (uint32_t)(time_us / 1000U);
    const unsigned int fraction = (unsigned int)(time_us % 1000U);
    /* ISRはraw記録だけ。mainの文字列整形も%fを使わず整数mAへ丸める。
     * Teleplotのキー・ms時刻・A単位/小数3桁は維持し、200Hz時のCPU負荷を減らす。 */
    int32_t ma[4];
    uint32_t magnitude[4];
    const char *sign[4];
    for (unsigned i=0;i<4;i++) {
      float value=((float)sample->raw[i]-zero_offsets[i])*scale*1000.0f;
      ma[i]=(int32_t)(value+(value<0.0f ? -0.5f : 0.5f));
      sign[i]=ma[i]<0 ? "-" : "";
      magnitude[i]=(uint32_t)(ma[i]<0 ? -ma[i] : ma[i]);
    }
    const int length = snprintf(tx_text, sizeof(tx_text),
      "\n>u1_a:%lu.%03u:%s%lu.%03lu\n>v_a:%lu.%03u:%s%lu.%03lu\n"
      ">u2_a:%lu.%03u:%s%lu.%03lu\n>w_a:%lu.%03u:%s%lu.%03lu\n"
      ">sector:%lu.%03u:%u\n>sample:%lu\n>log_overrun:%lu\n",
      ms,fraction,sign[0],(unsigned long)(magnitude[0]/1000U),(unsigned long)(magnitude[0]%1000U),
      ms,fraction,sign[1],(unsigned long)(magnitude[1]/1000U),(unsigned long)(magnitude[1]%1000U),
      ms,fraction,sign[2],(unsigned long)(magnitude[2]/1000U),(unsigned long)(magnitude[2]%1000U),
      ms,fraction,sign[3],(unsigned long)(magnitude[3]/1000U),(unsigned long)(magnitude[3]%1000U),
      ms,fraction,(unsigned int)sample->sector,
      (unsigned long)sample->sequence,(unsigned long)overruns);
    if (length <= 0 || (size_t)length >= sizeof(tx_text)) {
      Transmitted(false);
      return;
    }
    tx_length = (uint16_t)length;
  }
  transmitting = true;
  if (Console_TryTransmitBlock(tx_text, tx_length, Transmitted) != HAL_OK) {
    transmitting = false;
  }
}

bool DmaLogger_IsBusy(void)
{
  return running || states[0] != FREE || states[1] != FREE;
}

uint32_t DmaLogger_Overruns(void) { return overruns; }
