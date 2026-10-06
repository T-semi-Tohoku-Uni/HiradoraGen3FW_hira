#ifndef CURRENT_PI_H
#define CURRENT_PI_H

#include <stdbool.h>

typedef struct {
  float kp_d, ki_d, kp_q, ki_q;
  float integral_limit_d, integral_limit_q;
} CurrentPi_Config;

typedef struct {
  float integral_d, integral_q;
  float error_d, error_q;
  float vd, vq;
  bool saturated;
} CurrentPi_State;

void CurrentPi_Reset(CurrentPi_State *state);
bool CurrentPi_ConfigValid(const CurrentPi_Config *config);
/* Gains: Kp [V/A], Ki [V/(A*s)]; integrals and outputs [V], dt [s].
 * False leaves state unchanged. Call from one control ISR only. */
bool CurrentPi_Update(CurrentPi_State *state, const CurrentPi_Config *config,
                      float id_ref, float iq_ref, float id, float iq,
                      float dt, float voltage_limit);

#endif
