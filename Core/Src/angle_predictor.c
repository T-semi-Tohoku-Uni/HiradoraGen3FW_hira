#include "angle_predictor.h"
#include <math.h>
#define PI 3.14159265358979323846f
#define TWO_PI (2.0f*PI)
static float Wrap(float a)
{
  /* The observer normally stays within one turn of [0,2*pi). Avoid five
   * libm calls per ADC cycle; retain the general path for large inputs. */
  if (a>=0.0f && a<TWO_PI) return a;
  if (a>=TWO_PI && a<2.0f*TWO_PI) return a-TWO_PI;
  if (a<0.0f && a>=-TWO_PI) {
    a+=TWO_PI;
    return a<TWO_PI ? a : 0.0f;
  }
  a=fmodf(a,TWO_PI);
  if (a<0.0f) a+=TWO_PI;
  return a==TWO_PI ? 0.0f : a;
}
void AnglePredictor_Reset(AnglePredictor *state, float angle)
{
  state->angle=Wrap(angle);
  state->velocity=0.0f;
}
bool AnglePredictor_Update(AnglePredictor *state, float angle, float dt, float tau)
{
  if (!isfinite(angle) || !isfinite(dt) || dt<=0.0f || dt>0.001f ||
      !isfinite(tau) || tau<=0.0f || !isfinite(state->angle) ||
      !isfinite(state->velocity)) return false;
  float predicted=Wrap(state->angle+state->velocity*dt);
  float error=Wrap(angle)-predicted;
  if (error>PI) error-=TWO_PI;
  if (error< -PI) error+=TWO_PI;
  float g=dt/(tau+dt);
  state->angle=Wrap(predicted+(2.0f*g-g*g)*error);
  state->velocity+=g*g*error/dt;
  return isfinite(state->angle) && isfinite(state->velocity);
}
float AnglePredictor_At(const AnglePredictor *state, float dt)
{
  return Wrap(state->angle+state->velocity*dt);
}
