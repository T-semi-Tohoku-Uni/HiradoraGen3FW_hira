#include "voltage_vector.h"
#include <math.h>
#define TWO_PI 6.2831853071795864769f
float VoltageVector_Wrap(float angle)
{
  float value = fmodf(angle, TWO_PI);
  return value < 0.0f ? value + TWO_PI : value;
}
bool VoltageVector_Compute(float angle, float vd, float vq, float vm,
                           float limit, float margin, float duty[3])
{
  if (!duty || !isfinite(angle) || !isfinite(vd) || !isfinite(vq) ||
      !isfinite(vm) || !isfinite(limit) || !isfinite(margin) ||
      vm <= 0.0f || limit <= 0.0f || margin < 0.0f || margin >= 0.5f) return false;
  /* 円形制限でVd:Vqを保つ。線形SVPWM領域とbootstrap用余白の両方を守る。 */
  float magnitude = hypotf(vd, vq);
  if (!isfinite(magnitude)) return false;
  float maximum = fminf(limit, vm * (1.0f - 2.0f * margin) / 1.732050808f);
  if (magnitude > maximum) { float scale = maximum / magnitude; vd *= scale; vq *= scale; }
  float c = cosf(angle), s = sinf(angle);
  float alpha = vd * c - vq * s, beta = vd * s + vq * c;
  float u = alpha, v = -0.5f * alpha + 0.866025404f * beta;
  float w = -0.5f * alpha - 0.866025404f * beta;
  /* 三相の最大・最小の中点を引く共通モード注入。線間電圧は変えない。 */
  float common = 0.5f * (fmaxf(u, fmaxf(v,w)) + fminf(u, fminf(v,w)));
  duty[0] = 0.5f + (u-common)/vm;
  duty[1] = 0.5f + (v-common)/vm;
  duty[2] = 0.5f + (w-common)/vm;
  for (unsigned i=0; i<3; i++) duty[i] = fmaxf(margin, fminf(1.0f-margin, duty[i]));
  return true;
}
