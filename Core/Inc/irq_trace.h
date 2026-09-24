#ifndef IRQ_TRACE_H
#define IRQ_TRACE_H
/* 計測専用。各番号は開始/終了を対にする。DMA待ち時間はLAUNCH→RX開始で観測。 */
enum { TRACE_TIM_IN=1, TRACE_TIM_OUT, TRACE_ADC_IN, TRACE_ADC_OUT,
 TRACE_RX_IN, TRACE_RX_OUT, TRACE_LAUNCH, TRACE_READ, TRACE_PUBLISH };
void IrqTrace_Event(unsigned event);
void IrqTrace_Arm(void);
void IrqTrace_Begin(void);
void IrqTrace_Dump(void);
#endif
