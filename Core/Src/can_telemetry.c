#include "can_telemetry.h"

void CanTelemetry_Update(CanTelemetry *s, uint16_t raw, uint32_t cycles, uint32_t hz)
{
  if (!s) return;
  if (raw > 16383U || hz < 1000U) { *s = (CanTelemetry){0}; return; }
  s->angle = raw >> 1;
  uint32_t dt = cycles - s->previous_cycles;
  /* 10ms超の観測空白では周回方向を推測しない。再取得して窓を作り直す。
   * 10ms以下でも半回転以上の移動は識別不能。既存1000rpm上限内を想定。 */
  if (!s->tracking || dt > hz / 100U || dt == 0U) {
    s->counts = 0; s->window_cycles = 0; s->rpm = 0; s->speed_valid = false;
  } else {
    int32_t delta = (int32_t)raw - s->previous_raw;
    if (delta > 8191) delta -= 16384;
    if (delta < -8192) delta += 16384;
    s->counts += delta;
    s->window_cycles += dt;
    if (s->window_cycles >= hz / 100U) {
      float rpm = (float)s->counts * (60.0f / 16384.0f) * (float)hz / (float)s->window_cycles;
      if (rpm > 32767.0f) rpm = 32767.0f;
      if (rpm < -32768.0f) rpm = -32768.0f;
      s->rpm = (int16_t)rpm;
      s->speed_valid = true;
      s->counts = 0; s->window_cycles = 0;
    }
  }
  s->previous_raw = raw; s->previous_cycles = cycles; s->tracking = true;
}
