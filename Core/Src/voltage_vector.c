#include "voltage_vector.h"
#include <math.h>
#include "arm_math.h"
#if FOC_USE_CORDIC
#include "stm32g4xx_hal.h"
#include "stm32g4xx_ll_cordic.h"
#endif
#define TWO_PI 6.2831853071795864769f
void VoltageVector_Init(void)
{
#if FOC_USE_CORDIC
  __HAL_RCC_CORDIC_CLK_ENABLE();
  LL_CORDIC_Config(CORDIC, LL_CORDIC_FUNCTION_COSINE,
      LL_CORDIC_PRECISION_6CYCLES, LL_CORDIC_SCALE_0,
      LL_CORDIC_NBWRITE_2, LL_CORDIC_NBREAD_2,
      LL_CORDIC_INSIZE_32BITS, LL_CORDIC_OUTSIZE_32BITS);
#endif
}
float VoltageVector_Wrap(float angle)
{
  float value = fmodf(angle, TWO_PI);
  return value < 0.0f ? value + TWO_PI : value;
}
/* Normalise once before either backend. CMSIS takes degrees. */
void VoltageVector_SinCos(float angle, float *s, float *c)
{
  VoltageVector_SinCosWrapped(VoltageVector_Wrap(angle), s, c);
}
void VoltageVector_SinCosWrapped(float angle, float *s, float *c)
{
  /* Wrap can round a tiny negative remainder + 2*pi to exactly 2*pi.
   * Match the previous second fmodf at that endpoint. */
  if (angle == TWO_PI) angle = 0.0f;
#if FOC_USE_CORDIC
  /* CORDIC angle is radians/pi in signed Q1.31, in [-1,1).
   * Fold [pi,2*pi) to [-pi,0); guard float rounding before integer conversion. */
  if (!isfinite(angle)) { *s = NAN; *c = NAN; return; }
  if (angle >= 0.5f*TWO_PI) angle -= TWO_PI;
  float scaled = angle * (2147483648.0f / (0.5f*TWO_PI));
  int32_t input = scaled >= 2147483648.0f ? INT32_MAX :
      scaled <= -2147483648.0f ? INT32_MIN : (int32_t)scaled;
  /* Main calibration/test calls can be preempted by the ADC ISR. Protect
   * only the peripheral transaction; restore the caller's interrupt state.
   * RDATA reads stall until ready (zero-overhead mode). Always drain both. */
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  CORDIC->WDATA = (uint32_t)input;
  CORDIC->WDATA = 0x7fffffffU; /* unit modulus, supplied on every call */
  int32_t cosine = (int32_t)CORDIC->RDATA;
  int32_t sine = (int32_t)CORDIC->RDATA;
  __set_PRIMASK(primask);
  *c = (float)cosine * (1.0f/2147483648.0f);
  *s = (float)sine * (1.0f/2147483648.0f);
#else
  arm_sin_cos_f32(angle * (360.0f / TWO_PI), s, c);
#endif
}
bool VoltageVector_Compute(float angle, float vd, float vq, float vm,
                           float limit, float margin, float duty[3])
{
  if (!isfinite(angle)) return false;
  float s, c;
  VoltageVector_SinCos(angle, &s, &c);
  return VoltageVector_ComputeSinCos(s, c, vd, vq, vm, limit, margin, duty);
}
bool VoltageVector_ComputeSinCos(float s, float c, float vd, float vq, float vm,
                                float limit, float margin, float duty[3])
{
  if (!duty || !isfinite(s) || !isfinite(c) || !isfinite(vd) || !isfinite(vq) ||
      !isfinite(vm) || !isfinite(limit) || !isfinite(margin) ||
      vm <= 0.0f || limit <= 0.0f || margin < 0.0f || margin >= 0.5f) return false;
  /* 円形制限でVd:Vqを保つ。線形SVPWM領域とbootstrap用余白の両方を守る。 */
  /* 有限値を確認済み。単精度FPUの平方根と比較を使い、周期ISRのlibm呼出しを減らす。 */
  float magnitude = sqrtf(vd * vd + vq * vq);
  if (!isfinite(magnitude)) return false;
  float maximum = vm * (1.0f - 2.0f * margin) / 1.732050808f;
  if (maximum > limit) maximum = limit;
  if (magnitude > maximum) { float scale = maximum / magnitude; vd *= scale; vq *= scale; }
  float alpha = vd * c - vq * s, beta = vd * s + vq * c;
  float u = alpha, v = -0.5f * alpha + 0.866025404f * beta;
  float w = -0.5f * alpha - 0.866025404f * beta;
  /* 三相の最大・最小の中点を引く共通モード注入。線間電圧は変えない。 */
  float high = u > v ? u : v, low = u < v ? u : v;
  if (w > high) high = w;
  if (w < low) low = w;
  float common = 0.5f * (high + low);
  duty[0] = 0.5f + (u-common)/vm;
  duty[1] = 0.5f + (v-common)/vm;
  duty[2] = 0.5f + (w-common)/vm;
  for (unsigned i=0; i<3; i++) {
    if (duty[i] < margin) duty[i] = margin;
    if (duty[i] > 1.0f-margin) duty[i] = 1.0f-margin;
  }
  return true;
}
