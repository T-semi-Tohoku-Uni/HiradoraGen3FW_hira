#include "voltage_vector.h"
#include <math.h>
#include "arm_math.h"
#define TWO_PI 6.2831853071795864769f
float VoltageVector_Wrap(float angle)
{
  float value = fmodf(angle, TWO_PI);
  return value < 0.0f ? value + TWO_PI : value;
}
/* 同梱CMSIS-DSPのテーブル補間を使う。周辺設定やCORDICレジスタは変更しない。
 * CMSIS側は度単位なので、有限のラジアンを1回転内へ正規化してから渡す。 */
void VoltageVector_SinCos(float angle, float *s, float *c)
{
  float wrapped = VoltageVector_Wrap(angle);
  arm_sin_cos_f32(wrapped * (360.0f / TWO_PI), s, c);
}
bool VoltageVector_Compute(float angle, float vd, float vq, float vm,
                           float limit, float margin, float duty[3])
{
  if (!duty || !isfinite(angle) || !isfinite(vd) || !isfinite(vq) ||
      !isfinite(vm) || !isfinite(limit) || !isfinite(margin) ||
      vm <= 0.0f || limit <= 0.0f || margin < 0.0f || margin >= 0.5f) return false;
  /* 円形制限でVd:Vqを保つ。線形SVPWM領域とbootstrap用余白の両方を守る。 */
  /* 有限値を確認済み。単精度FPUの平方根と比較を使い、周期ISRのlibm呼出しを減らす。 */
  float magnitude = sqrtf(vd * vd + vq * vq);
  if (!isfinite(magnitude)) return false;
  float maximum = vm * (1.0f - 2.0f * margin) / 1.732050808f;
  if (maximum > limit) maximum = limit;
  if (magnitude > maximum) { float scale = maximum / magnitude; vd *= scale; vq *= scale; }
  float c, s;
  VoltageVector_SinCos(angle, &s, &c);
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
