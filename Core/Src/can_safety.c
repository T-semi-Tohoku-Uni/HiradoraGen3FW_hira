#include "can_safety.h"
bool CanSafety_Arm(volatile CanSafety *s, uint32_t now)
{
  if (s->state == CAN_SAFE_WAIT_ZERO || s->state == CAN_SAFE_READY) return false;
  s->command = 0; s->reason = CAN_TRIP_NONE; s->last_ms = now;
  s->state = CAN_SAFE_WAIT_ZERO;
  return true;
}
void CanSafety_Disarm(volatile CanSafety *s)
{
  s->state = CAN_SAFE_DISARMED; s->reason = CAN_TRIP_NONE; s->command = 0;
}
void CanSafety_Trip(volatile CanSafety *s, uint32_t reason)
{
  if (s->state == CAN_SAFE_WAIT_ZERO || s->state == CAN_SAFE_READY) {
    s->state = CAN_SAFE_FAULT; s->reason = reason; s->command = 0;
  }
}
void CanSafety_Tick(volatile CanSafety *s, uint32_t now)
{
  if ((uint32_t)(now - s->last_ms) >= 50U) CanSafety_Trip(s, CAN_TRIP_TIMEOUT);
}
bool CanSafety_Receive(volatile CanSafety *s, int32_t raw, uint32_t received_ms, uint32_t now)
{
  /* 期限後に届いたフレームでタイムアウトを隠さない。 */
  CanSafety_Tick(s, now);
  if (s->state != CAN_SAFE_WAIT_ZERO && s->state != CAN_SAFE_READY) return false;
  if ((uint32_t)(now - received_ms) > 5U) { CanSafety_Trip(s, CAN_TRIP_STALE); return false; }
  if (s->state == CAN_SAFE_WAIT_ZERO && raw != 0) return false;
  s->command = raw; s->last_ms = received_ms; s->state = CAN_SAFE_READY;
  return true;
}
