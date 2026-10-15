#ifndef CAN_SAFETY_H
#define CAN_SAFETY_H
#include <stdbool.h>
#include <stdint.h>
enum { CAN_SAFE_DISARMED, CAN_SAFE_WAIT_ZERO, CAN_SAFE_READY, CAN_SAFE_FAULT };
enum { CAN_TRIP_NONE, CAN_TRIP_TIMEOUT, CAN_TRIP_FIFO, CAN_TRIP_BUS_OFF,
       CAN_TRIP_STALE, CAN_TRIP_GATE, CAN_TRIP_RX };
typedef struct {
  uint32_t state, reason, last_ms;
  int32_t command;
} CanSafety;
/* 呼出し側で短い排他区間を設ける。HAL非依存の状態遷移。 */
bool CanSafety_Arm(volatile CanSafety *s, uint32_t now);
void CanSafety_Disarm(volatile CanSafety *s);
void CanSafety_Trip(volatile CanSafety *s, uint32_t reason);
void CanSafety_Tick(volatile CanSafety *s, uint32_t now);
bool CanSafety_Receive(volatile CanSafety *s, int32_t raw, uint32_t received_ms, uint32_t now);
#endif
