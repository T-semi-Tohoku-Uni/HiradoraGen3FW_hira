#ifndef ANGLE_PREDICTOR_H
#define ANGLE_PREDICTOR_H
#include <stdbool.h>
typedef struct {
  float angle, velocity;
} AnglePredictor;
/* Angles/velocity are in the calibrated electrical coordinate system. */
void AnglePredictor_Reset(AnglePredictor *state, float angle);
bool AnglePredictor_Update(AnglePredictor *state, float angle, float dt, float tau);
float AnglePredictor_At(const AnglePredictor *state, float dt);
#endif
