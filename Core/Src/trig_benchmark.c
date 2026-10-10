#include "voltage_vector.h"
#include "stm32g4xx_hal.h"
#include "arm_math.h"
#include <math.h>
#include <stdio.h>

#define TWO_PI 6.2831853071795864769f
typedef void (*PairFunction)(float, float *, float *);
static void Reference(float angle, float *s, float *c)
{
  if (angle == TWO_PI) angle = 0.0f;
  arm_sin_cos_f32(angle*(360.0f/TWO_PI), s, c);
}
typedef struct { uint32_t sum, min, max; } Timing;
static Timing Measure(PairFunction function)
{
  Timing result = {0, UINT32_MAX, 0};
  float s, c;
  volatile float sink = 0.0f;
  for (unsigned i=0; i<1024; i++) {
    float angle = (float)i*TWO_PI/1024.0f;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    __DSB(); __ISB();
    uint32_t start = DWT->CYCCNT;
    function(angle, &s, &c);
    uint32_t elapsed = DWT->CYCCNT-start;
    __set_PRIMASK(primask);
    sink = s+c;
    result.sum += elapsed;
    if (elapsed < result.min) result.min = elapsed;
    if (elapsed > result.max) result.max = elapsed;
  }
  (void)sink;
  return result;
}
void VoltageVector_TrigTest(void)
{
  /* This diagnostic never writes PWM or calibration Flash. */
  const float edge[] = {0.0f, -0.0f, -1e-8f, TWO_PI, TWO_PI-5e-7f,
      0.5f*TWO_PI, 0.5f*TWO_PI-3e-7f, 0.5f*TWO_PI+3e-7f};
  const float conditions[][3] = {{.3f,-.2f,24}, {0,0,24}, {100,-100,6}};
  unsigned failures=0, checks=0;
  float pair_error=0, duty_error=0, norm_error=0;
  for (unsigned i=0; i<1025+sizeof(edge)/sizeof(edge[0]); i++) {
    float angle=VoltageVector_Wrap(i<1025 ? (float)i*TWO_PI/1024.0f : edge[i-1025]);
    float s,c,rs,rc;
    VoltageVector_SinCosWrapped(angle,&s,&c);
    Reference(angle,&rs,&rc);
    float error=fmaxf(fabsf(s-rs),fabsf(c-rc));
    float norm=fabsf(s*s+c*c-1.0f);
    if (error>pair_error) pair_error=error;
    if (norm>norm_error) norm_error=norm;
    checks++;
    if (!isfinite(s) || !isfinite(c) || error>5e-6f || norm>1e-5f) failures++;
    for (unsigned j=0;j<3;j++) {
      float d[3],r[3];
      bool ok=VoltageVector_ComputeSinCos(s,c,conditions[j][0],conditions[j][1],conditions[j][2],3,.05f,d);
      bool rok=VoltageVector_ComputeSinCos(rs,rc,conditions[j][0],conditions[j][1],conditions[j][2],3,.05f,r);
      checks++;
      if (!ok || !rok) { failures++; continue; }
      for (unsigned k=0;k<3;k++) {
        float delta=fabsf(d[k]-r[k]);
        if (delta>duty_error) duty_error=delta;
        if (!isfinite(d[k]) || delta>3e-6f) failures++;
      }
    }
  }
  SET_BIT(CoreDebug->DEMCR, CoreDebug_DEMCR_TRCENA_Msk);
  SET_BIT(DWT->CTRL, DWT_CTRL_CYCCNTENA_Msk);
  __DSB(); __ISB();
  /* Warm both paths; measurement includes conversions/call overhead and
   * CORDIC's own PRIMASK save/restore, excludes external interrupt service. */
  (void)Measure(Reference); (void)Measure(VoltageVector_SinCosWrapped);
  Timing reference=Measure(Reference), selected=Measure(VoltageVector_SinCosWrapped);
  printf("TRIG_TEST backend=%s checks=%u failures=%u pair_error=%.9f duty_error=%.9f norm_error=%.9f\r\n",
      FOC_USE_CORDIC ? "CORDIC" : "CMSIS",checks,failures,
      (double)pair_error,(double)duty_error,(double)norm_error);
  printf("TRIG_CYCLES n=1024 clock=%lu reference_sum=%lu min=%lu max=%lu selected_sum=%lu min=%lu max=%lu\r\n",
      (unsigned long)SystemCoreClock,(unsigned long)reference.sum,
      (unsigned long)reference.min,(unsigned long)reference.max,
      (unsigned long)selected.sum,(unsigned long)selected.min,(unsigned long)selected.max);
}
