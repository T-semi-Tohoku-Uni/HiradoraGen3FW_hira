#ifndef CAN_PROTOCOL_H
#define CAN_PROTOCOL_H
#include <stdbool.h>
#include <stdint.h>
/* 校正用MOTOR_CONTROL_MOTOR_IDとは独立したCANノード番号。 */
#define CAN_NODE_ID 1U
bool CanProtocol_Decode(uint8_t node, uint32_t id, const uint8_t data[8],
                        uint8_t length, int16_t *current);
bool CanProtocol_Pack(uint8_t node, uint16_t angle, int16_t rpm,
                      int16_t iq, uint8_t temperature, uint32_t *id, uint8_t data[8]);
#endif
