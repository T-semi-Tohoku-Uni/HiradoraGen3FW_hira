#ifndef VOLTAGE_VECTOR_H
#define VOLTAGE_VECTOR_H
#include <stdbool.h>
/* 位相順UVWの電気角[rad]。Vd/Vqは相電圧ベクトル[V]。 */
bool VoltageVector_Compute(float angle, float vd, float vq, float vm,
                           float limit, float margin, float duty[3]);
float VoltageVector_Wrap(float angle);
#endif
