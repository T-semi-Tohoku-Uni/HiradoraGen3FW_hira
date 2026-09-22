#ifndef CURRENT_SENSE_H
#define CURRENT_SENSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

#include <stdbool.h>

/**
 * @brief Prepare OPAMPs/ADCs and measure startup zero-current offsets.
 *
 * ADC1 is expected to be the master and ADC2 the slave of the injected
 * simultaneous conversion configured by CubeMX.
 * Call before enabling motor PWM, with zero phase current. Blocks until
 * 64 VREFINT/VM pairs and 1000 zero-current sample sets are captured
 * (or timeout). ADC1 regular rank 1 must be VREFINT with adequate sampling
 * time; regular rank 2 must be PC2/IN8, discontinuous one rank per start. Calibration accumulates sums without storing samples. Streaming formats calibrated currents in main context and sends
 * standard Teleplot serial text over UART DMA.
 */
HAL_StatusTypeDef CurrentSense_Init(ADC_HandleTypeDef *master_adc,
                                    ADC_HandleTypeDef *slave_adc,
                                    OPAMP_HandleTypeDef *u_opamp,
                                    OPAMP_HandleTypeDef *v_opamp,
                                    OPAMP_HandleTypeDef *w_opamp,
                                    TIM_HandleTypeDef *trigger_timer);

/**
 * @brief Handle adc [decimation], adc stop, and adc status.
 * @return true if the command belongs to this module, otherwise false.
 */
bool CurrentSense_ProcessCommand(const char *command);

/** @brief Service Teleplot DMA transmission and acquisition errors from main. */
void CurrentSense_Task(void);

/** @brief Return true while acquisition or DMA draining is in progress. */
bool CurrentSense_IsBusy(void);

#ifdef __cplusplus
}
#endif

#endif /* CURRENT_SENSE_H */
