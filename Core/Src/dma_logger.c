#include "dma_logger.h"
#include "console.h"
#include "foc_voltage.h"
#include <string.h>

typedef struct {
  uint32_t sequence;
  uint16_t raw[4];
  uint16_t sector;
  bool foc_valid;
  FocVoltage_Observation foc;
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
static char tx_text[640];
static uint16_t tx_length;

/* Teleplot専用の整数整形。多数のsnprintfによるmain負荷を避ける。
 * すべての書込みを境界確認し、切詰めを正常なログとして送らない。ISRでは使わない。 */
typedef struct { char *position, *end; bool ok; } TextWriter;
static void PutChar(TextWriter *out,char value)
{
  if (out->position==out->end) { out->ok=false; return; }
  *out->position++=value;
}
static void PutText(TextWriter *out,const char *text)
{
  while (*text) PutChar(out,*text++);
}
static void PutUnsigned(TextWriter *out,uint32_t value,unsigned width)
{
  char digits[10]; unsigned count=0;
  do { digits[count++]=(char)('0'+value%10U); value/=10U; } while(value);
  while(width>count) { PutChar(out,'0'); width--; }
  while(count) PutChar(out,digits[--count]);
}
static void PutPrefix(TextWriter *out,const char *key,const char *timestamp)
{
  PutChar(out,'>'); PutText(out,key); PutChar(out,':');
  if (timestamp) { PutText(out,timestamp); PutChar(out,':'); }
}
static void PutInteger(TextWriter *out,const char *key,const char *timestamp,uint32_t value)
{
  PutPrefix(out,key,timestamp); PutUnsigned(out,value,1U); PutChar(out,'\n');
}
static void PutFixed(TextWriter *out,const char *key,const char *timestamp,float value)
{
  float scaled=value*1000.0f;
  int32_t rounded=(int32_t)(scaled+(scaled<0.0f ? -0.5f : 0.5f));
  uint32_t absolute=(uint32_t)(rounded<0 ? -rounded : rounded);
  PutPrefix(out,key,timestamp);
  if (rounded<0) PutChar(out,'-');
  PutUnsigned(out,absolute/1000U,1U); PutChar(out,'.');
  PutUnsigned(out,absolute%1000U,3U); PutChar(out,'\n');
}


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
  /* 間引き対象だけコピー。mainの最新値ではADCと別周期になる。 */
  sample->foc_valid = FocVoltage_GetObservationISR(&sample->foc);
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
    /* 相手ブロックが送信待ちなら、現在の空きへ収集を続ける。
     * 小さな端数を両方READYにしてしまうと空き容量があっても欠落する。 */
    if (states[producer] == FILLING && states[producer ^ 1U] == FREE) Seal();
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
    char timestamp[16];
    TextWriter stamp={timestamp,timestamp+sizeof(timestamp)-1U,true};
    PutUnsigned(&stamp,ms,1U); PutChar(&stamp,'.'); PutUnsigned(&stamp,fraction,3U);
    *stamp.position='\0';
    TextWriter out={tx_text,tx_text+sizeof(tx_text),true};
    PutChar(&out,'\n');
    static const char *const keys[4]={"u1_a","v_a","u2_a","w_a"};
    for (unsigned i=0;i<4;i++)
      PutFixed(&out,keys[i],timestamp,((float)sample->raw[i]-zero_offsets[i])*scale);
    PutInteger(&out,"sector",timestamp,sample->sector);
    PutInteger(&out,"sample",NULL,sample->sequence);
    PutInteger(&out,"log_overrun",NULL,overruns);
    if (sample->foc_valid) {
      PutFixed(&out,"id_a",timestamp,sample->foc.id_a);
      PutFixed(&out,"iq_a",timestamp,sample->foc.iq_a);
      PutFixed(&out,"elec_rad",timestamp,sample->foc.electrical_rad);
      /* unsigned差分でDWT周回を跨ぐ。真のセンサー測定遅延ではない。 */
      uint32_t request_age=sample->foc.adc_callback_cycles-sample->foc.angle_request_cycles;
      uint32_t received_age=sample->foc.adc_callback_cycles-sample->foc.angle_received_cycles;
      PutInteger(&out,"angle_age_us",timestamp,request_age/(SystemCoreClock/1000000U));
      PutInteger(&out,"angle_rx_age_us",timestamp,received_age/(SystemCoreClock/1000000U));
    }
    if (!out.ok || !stamp.ok) { Transmitted(false); return; }
    tx_length=(uint16_t)(out.position-tx_text);
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
