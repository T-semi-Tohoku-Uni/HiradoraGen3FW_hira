#ifndef CALIBRATION_STORE_H
#define CALIBRATION_STORE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
  uint32_t version, motor_id, pole_pairs;
  float kv;
  int32_t direction;
  float offset;
  uint32_t magic, crc;
} CalibrationRecord;
void CalibrationStore_Make(CalibrationRecord *record, int32_t direction, float offset);
bool CalibrationStore_Valid(const CalibrationRecord *record);
bool CalibrationStore_Load(CalibrationRecord *record);
bool CalibrationStore_Save(const CalibrationRecord *record);
#endif
