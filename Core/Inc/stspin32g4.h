#ifndef STSPIN32G4_H
#define STSPIN32G4_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

#include <stdbool.h>
#include <stdint.h>

/* Fixed 7-bit gate-driver address from STSPIN32G4 datasheet table 16. */
#define STSPIN32G4_I2C_ADDRESS_7BIT       0x47U

#define STSPIN32G4_STATUS_LOCK            (1U << 7)
#define STSPIN32G4_STATUS_RESET           (1U << 3)
#define STSPIN32G4_STATUS_VDS_PROTECTION  (1U << 2)
#define STSPIN32G4_STATUS_THERMAL_SHUTDOWN (1U << 1)
#define STSPIN32G4_STATUS_VCC_UVLO        (1U << 0)

#define STSPIN32G4_STATUS_FAULT_MASK                              \
  (STSPIN32G4_STATUS_RESET | STSPIN32G4_STATUS_VDS_PROTECTION |  \
   STSPIN32G4_STATUS_THERMAL_SHUTDOWN | STSPIN32G4_STATUS_VCC_UVLO)

typedef struct
{
  uint8_t before_clear;
  uint8_t after_clear;
  bool clear_requested;
} STSPIN32G4_FaultReport;

/** @brief Read the read-only STATUS register (address 0x80). */
HAL_StatusTypeDef STSPIN32G4_ReadStatus(I2C_HandleTypeDef *hi2c,
                                       uint8_t *status);

/** @brief Write 0xFF to the CLEAR command register (address 0x09). */
HAL_StatusTypeDef STSPIN32G4_ClearFaults(I2C_HandleTypeDef *hi2c);

/**
 * @brief Read STATUS, clear reported faults, wait, and read STATUS again.
 *
 * RESET and VDS protection are latched faults. Thermal shutdown and VCC UVLO
 * only disappear after their physical causes are removed.
 */
HAL_StatusTypeDef STSPIN32G4_CheckAndClearFaults(
  I2C_HandleTypeDef *hi2c,
  STSPIN32G4_FaultReport *report);

/** @return true if RESET, VDS, THSD, or VCC_UVLO is present. */
bool STSPIN32G4_StatusHasFault(uint8_t status);

#ifdef __cplusplus
}
#endif

#endif /* STSPIN32G4_H */
