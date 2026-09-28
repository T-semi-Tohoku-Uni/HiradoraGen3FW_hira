#include "stspin32g4.h"

#define STSPIN32G4_HAL_DEVICE_ADDRESS \
  (STSPIN32G4_I2C_ADDRESS_7BIT << 1U)

#define STSPIN32G4_REGISTER_CLEAR   0x09U
#define STSPIN32G4_REGISTER_STATUS  0x80U
#define STSPIN32G4_CLEAR_ALL_VALUE  0xFFU
#define STSPIN32G4_I2C_TIMEOUT_MS   20U
#define STSPIN32G4_CLEAR_WAIT_MS    1U

HAL_StatusTypeDef STSPIN32G4_ReadStatus(I2C_HandleTypeDef *hi2c,
                                       uint8_t *status)
{
  if ((hi2c == NULL) || (status == NULL))
  {
    return HAL_ERROR;
  }

  return HAL_I2C_Mem_Read(hi2c,
                          STSPIN32G4_HAL_DEVICE_ADDRESS,
                          STSPIN32G4_REGISTER_STATUS,
                          I2C_MEMADD_SIZE_8BIT,
                          status,
                          1U,
                          STSPIN32G4_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef STSPIN32G4_ClearFaults(I2C_HandleTypeDef *hi2c)
{
  uint8_t clear_command = STSPIN32G4_CLEAR_ALL_VALUE;

  if (hi2c == NULL)
  {
    return HAL_ERROR;
  }

  return HAL_I2C_Mem_Write(hi2c,
                           STSPIN32G4_HAL_DEVICE_ADDRESS,
                           STSPIN32G4_REGISTER_CLEAR,
                           I2C_MEMADD_SIZE_8BIT,
                           &clear_command,
                           1U,
                           STSPIN32G4_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef STSPIN32G4_CheckAndClearFaults(
  I2C_HandleTypeDef *hi2c,
  STSPIN32G4_FaultReport *report)
{
  HAL_StatusTypeDef result;

  if ((hi2c == NULL) || (report == NULL))
  {
    return HAL_ERROR;
  }

  report->before_clear = 0U;
  report->after_clear = 0U;
  report->clear_requested = false;

  result = STSPIN32G4_ReadStatus(hi2c, &report->before_clear);
  if (result != HAL_OK)
  {
    return result;
  }

  report->after_clear = report->before_clear;
  if (!STSPIN32G4_StatusHasFault(report->before_clear))
  {
    return HAL_OK;
  }

  result = STSPIN32G4_ClearFaults(hi2c);
  if (result != HAL_OK)
  {
    return result;
  }

  report->clear_requested = true;

  /* Datasheet tFAULT,reset is 160 us; one HAL tick gives sufficient margin. */
  HAL_Delay(STSPIN32G4_CLEAR_WAIT_MS);

  return STSPIN32G4_ReadStatus(hi2c, &report->after_clear);
}

bool STSPIN32G4_StatusHasFault(uint8_t status)
{
  return ((status & STSPIN32G4_STATUS_FAULT_MASK) != 0U);
}
