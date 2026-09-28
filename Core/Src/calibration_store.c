#include "calibration_store.h"
#include "motor_control_config.h"
#include "stm32g4xx_hal.h"
#include "motor_control.h"
#include "current_sense.h"
#include <math.h>
#include <stddef.h>
#include <string.h>
/* 最終2KiBをリンカで予約。アドレスをリンク定義と二重に固定しない。 */
extern const uint8_t __calibration_start__[];
#define CAL_MAGIC 0x43414C31U
_Static_assert(sizeof(CalibrationRecord) == 32U, "Flash record layout");
_Static_assert(FLASH_PAGE_SIZE == 2048U, "Requires STM32G431 2KiB pages");
static uint32_t Crc(const void *data, size_t length)
{
  const uint8_t *p = data;
  uint32_t crc = 0xFFFFFFFFU;
  while (length--) {
    crc ^= *p++;
    for (unsigned i=0; i<8; i++) crc = (crc>>1) ^ ((0U-(crc&1U)) & 0xEDB88320U);
  }
  return ~crc;
}
void CalibrationStore_Make(CalibrationRecord *r, int32_t direction, float offset)
{
  *r = (CalibrationRecord){1U, MOTOR_CONTROL_MOTOR_ID, MOTOR_CONTROL_POLE_PAIRS,
      MOTOR_CONTROL_KV_RPM_PER_VOLT, direction, offset, CAL_MAGIC, 0U};
  r->crc = Crc(r, offsetof(CalibrationRecord, crc));
}
bool CalibrationStore_Valid(const CalibrationRecord *r)
{
  return r && r->magic == CAL_MAGIC && r->version == 1U &&
    r->motor_id == MOTOR_CONTROL_MOTOR_ID && r->pole_pairs == MOTOR_CONTROL_POLE_PAIRS &&
    isfinite(r->kv) && r->kv == MOTOR_CONTROL_KV_RPM_PER_VOLT &&
    (r->direction == 1 || r->direction == -1) && isfinite(r->offset) &&
    r->offset >= 0.0f && r->offset < 6.2831853071795864769f &&
    r->crc == Crc(r, offsetof(CalibrationRecord, crc));
}
bool CalibrationStore_Load(CalibrationRecord *r)
{
  memcpy(r, __calibration_start__, sizeof(*r));
  return CalibrationStore_Valid(r);
}
bool CalibrationStore_Save(const CalibrationRecord *r)
{
  /* 同一bankへの消去中はCPUが停止し得る。必ず全PWM/ADCログ停止後にmainで呼ぶ。
   * 最後のdoubleword（magic/CRC）まで書けたレコードだけを起動時に採用する。
   * 電断時は旧値も失う方式。CRC不一致なら自動校正せず、手動再校正を要求する。 */
  if (!CalibrationStore_Valid(r) || !MotorControl_IsStopped() || CurrentSense_IsBusy()) return false;
  uint32_t address = (uint32_t)__calibration_start__;
  if (address != FLASH_BASE + 126U*1024U) return false;
  FLASH_EraseInitTypeDef erase = {0};
  erase.TypeErase = FLASH_TYPEERASE_PAGES; erase.Banks = FLASH_BANK_1;
  erase.Page = (address-FLASH_BASE)/FLASH_PAGE_SIZE; erase.NbPages = 1;
  uint32_t error;
  if (HAL_FLASH_Unlock() != HAL_OK) return false;
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
  HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &error);
  for (unsigned i=0; status == HAL_OK && i<sizeof(*r); i+=8U) {
    uint64_t word;
    memcpy(&word, (const uint8_t *)r+i, sizeof(word));
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, address+i, word);
  }
  (void)HAL_FLASH_Lock();
  CalibrationRecord check;
  return status == HAL_OK && CalibrationStore_Load(&check) && memcmp(r,&check,sizeof(check)) == 0;
}
