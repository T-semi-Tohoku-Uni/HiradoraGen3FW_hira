#ifndef H2_FIT_H
#define H2_FIT_H
#include <stdbool.h>
#include <stdint.h>
/* Main-context streaming least squares; no point array and no ISR work. */
typedef struct {
  double x, y, xx, xy, yy, e, xe, ye;
  float previous_error;
  uint32_t count;
  bool invalid;
} H2Fit;
typedef struct { float c, a, b; } H2FitResult;
void H2Fit_Add(H2Fit *fit, float theta_m, float error_e);
bool H2Fit_Solve(const H2Fit *fit, H2FitResult *result);
#endif
