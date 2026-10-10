#ifndef VOLTAGE_VECTOR_H
#define VOLTAGE_VECTOR_H
#include <stdbool.h>
/* 位相順UVWの電気角[rad]。Vd/Vqは相電圧ベクトル[V]。 */
bool VoltageVector_Compute(float angle, float vd, float vq, float vm,
                           float limit, float margin, float duty[3]);
void VoltageVector_SinCos(float angle, float *s, float *c);
/* Call once before enabling ADC/PWM interrupts. */
void VoltageVector_Init(void);
/* Stopped-only diagnostic; prints numerical and DWT timing comparison. */
void VoltageVector_TrigTest(void);
/* Finite angle already returned by Wrap; includes its rounded 2*pi endpoint. */
void VoltageVector_SinCosWrapped(float angle, float *s, float *c);
/* s/c must be the sine/cosine pair of one angle, not an arbitrary vector. */
bool VoltageVector_ComputeSinCos(float s, float c, float vd, float vq, float vm,
                                float limit, float margin, float duty[3]);
float VoltageVector_Wrap(float angle);
#endif
