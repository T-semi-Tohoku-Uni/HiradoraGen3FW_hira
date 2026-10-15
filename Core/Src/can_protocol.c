#include "can_protocol.h"
#include <stddef.h>

bool CanProtocol_Decode(uint8_t node, uint32_t id, const uint8_t data[8],
                        uint8_t length, int16_t *current)
{
  if (node < 1U || node > 8U || !data || !current || length != 8U ||
      id != (node <= 4U ? 0x200U : 0x1FFU)) return false;
  unsigned offset = 2U * ((node - 1U) % 4U);
  uint32_t raw = ((uint32_t)data[offset] << 8) | data[offset + 1U];
  /* 範囲外unsignedからsignedへの処理系依存変換を避ける。 */
  *current = (int16_t)(raw >= 32768U ? (int32_t)raw - 65536 : (int32_t)raw);
  return true;
}

static void Put16(uint8_t *data, uint16_t value)
{
  data[0] = (uint8_t)(value >> 8);
  data[1] = (uint8_t)value;
}

bool CanProtocol_Pack(uint8_t node, uint16_t angle, int16_t rpm,
                      int16_t iq, uint8_t temperature, uint32_t *id, uint8_t data[8])
{
  if (node < 1U || node > 8U || angle > 8191U || !id || !data) return false;
  *id = 0x200U + node;
  Put16(data, angle);
  Put16(data + 2, (uint16_t)rpm);
  Put16(data + 4, (uint16_t)iq);
  data[6] = temperature;
  data[7] = 0U;
  return true;
}
