#ifndef CAN_TELEMETRY_H
#define CAN_TELEMETRY_H
#include <stdbool.h>
#include <stdint.h>
/* main専用。正方向はAS5047Pの生機械角増加方向（校正方向とは独立）。 */
typedef struct {
  uint32_t previous_cycles, window_cycles;
  int32_t counts;
  uint16_t previous_raw, angle;
  int16_t rpm;
  bool tracking, speed_valid;
} CanTelemetry;
void CanTelemetry_Update(CanTelemetry *state, uint16_t raw, uint32_t cycles, uint32_t clock_hz);
#endif
