#ifndef CALIBRATION_STORE_H
#define CALIBRATION_STORE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
  uint32_t version, motor_id, pole_pairs;
  float kv;
  int32_t direction;
  float offset;
  float h2_cos_rad_elec, h2_sin_rad_elec;
  uint32_t magic, crc;
} CalibrationRecord;
void CalibrationStore_Make(CalibrationRecord *record, int32_t direction, float offset);
void CalibrationStore_SetH2(CalibrationRecord *record, float a2, float b2);
/* False for a migrated v1 record: RAM is valid v2, but not yet saved as v2. */
bool CalibrationStore_IsCurrentFormat(void);
bool CalibrationStore_Valid(const CalibrationRecord *record);
bool CalibrationStore_Load(CalibrationRecord *record);
bool CalibrationStore_Save(const CalibrationRecord *record);
#endif
