#include "current_pi.h"
#include <math.h>

void CurrentPi_Reset(CurrentPi_State *state)
{
  *state = (CurrentPi_State){0};
}

bool CurrentPi_ConfigValid(const CurrentPi_Config *c)
{
  return c && isfinite(c->kp_d) && c->kp_d >= 0.0f &&
    isfinite(c->ki_d) && c->ki_d >= 0.0f &&
    isfinite(c->kp_q) && c->kp_q >= 0.0f &&
    isfinite(c->ki_q) && c->ki_q >= 0.0f &&
    isfinite(c->integral_limit_d) && c->integral_limit_d > 0.0f &&
    isfinite(c->integral_limit_q) && c->integral_limit_q > 0.0f;
}

static float Clamp(float x, float limit)
{
  return x > limit ? limit : (x < -limit ? -limit : x);
}

static bool Limit(float *d, float *q, float limit, bool *saturated)
{
  float magnitude = sqrtf(*d * *d + *q * *q);
  if (!isfinite(magnitude)) return false;
  *saturated = magnitude > limit;
  if (*saturated) {
    float scale = limit / magnitude;
    *d *= scale;
    *q *= scale;
  }
  return true;
}

bool CurrentPi_Update(CurrentPi_State *state, const CurrentPi_Config *c,
                      float id_ref, float iq_ref, float id, float iq,
                      float dt, float voltage_limit)
{
  if (!state || !CurrentPi_ConfigValid(c) || !isfinite(id_ref) ||
      !isfinite(iq_ref) || !isfinite(id) || !isfinite(iq) ||
      !isfinite(dt) || dt <= 0.0f || !isfinite(voltage_limit) ||
      voltage_limit <= 0.0f) return false;
  CurrentPi_State next = *state;
  next.error_d = id_ref - id;
  next.error_q = iq_ref - iq;
  float di = c->ki_d * next.error_d * dt;
  float qi = c->ki_q * next.error_q * dt;
  float raw_id = state->integral_d + di;
  float raw_iq = state->integral_q + qi;
  if (!isfinite(raw_id) || !isfinite(raw_iq)) return false;
  next.integral_d = Clamp(raw_id, c->integral_limit_d);
  next.integral_q = Clamp(raw_iq, c->integral_limit_q);
  float pd = c->kp_d * next.error_d, pq = c->kp_q * next.error_q;
  float raw_d = pd + next.integral_d, raw_q = pq + next.integral_q;
  next.vd = raw_d;
  next.vq = raw_q;
  if (!Limit(&next.vd, &next.vq, voltage_limit, &next.saturated)) return false;
  if (next.saturated) {
    /* Reject only integral increments that deepen vector saturation.
     * Increments toward recovery remain enabled. */
    if ((raw_d - next.vd) * di > 0.0f) next.integral_d = state->integral_d;
    if ((raw_q - next.vq) * qi > 0.0f) next.integral_q = state->integral_q;
    next.vd = pd + next.integral_d;
    next.vq = pq + next.integral_q;
    bool limited;
    if (!Limit(&next.vd, &next.vq, voltage_limit, &limited)) return false;
  }
  *state = next;
  return true;
}
