#ifndef CAN_TELEMETRY_H
#define CAN_TELEMETRY_H
#include <stdbool.h>
#include <stdint.h>
/* main only. Input is normalized to 14 bits/turn; AS5600 raw is shifted by 2.
 * Positive is increasing raw mechanical angle, independent of calibration. */
typedef struct {
  uint32_t previous_cycles, window_cycles;
  int32_t counts;
  uint16_t previous_raw, angle;
  int16_t rpm;
  bool tracking, speed_valid;
} CanTelemetry;
void CanTelemetry_Update(CanTelemetry *state, uint16_t raw, uint32_t cycles, uint32_t clock_hz);
#endif
