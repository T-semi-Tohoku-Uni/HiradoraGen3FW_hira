#include "irq_trace.h"
#include "main.h"
#include <stdbool.h>
#include <stdio.h>
/* 最初の256イベントで自動停止。短い排他で入れ子ISRの記録順序を保持する。
 * 実行時の文字列整形は禁止。DWTはリセットしない。計測自体の負荷は残る。 */
static struct { uint32_t cycle, event; } entries[256];
static volatile unsigned count;
static volatile bool armed, active;
void IrqTrace_Arm(void) { active=false; count=0; armed=true; }
void IrqTrace_Begin(void) { if(armed) { armed=false; active=true; } }
void IrqTrace_Event(unsigned event)
{
 if(!active) return;
 uint32_t mask=__get_PRIMASK(); __disable_irq();
 unsigned i=count;
 if(i<256) { entries[i].cycle=DWT->CYCCNT; entries[i].event=event; count=i+1; }
 if(count==256) active=false;
 __set_PRIMASK(mask);
}
void IrqTrace_Dump(void)
{
 active=false; armed=false;
 printf("TRACE count=%u clock=%lu\r\n",count,(unsigned long)SystemCoreClock);
 for(unsigned i=0;i<count;i++) printf("TRACE %lu %lu\r\n",
 (unsigned long)(entries[i].cycle-entries[0].cycle),(unsigned long)entries[i].event);
}
