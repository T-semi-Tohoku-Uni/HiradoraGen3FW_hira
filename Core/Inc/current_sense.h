#ifndef CURRENT_SENSE_H
#define CURRENT_SENSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

#include <stdbool.h>

#define CURRENT_SENSE_SAMPLE_COUNT 4000U

/**
 * @brief Prepare OPAMPs/ADCs and measure startup zero-current offsets.
 *
 * ADC1 is expected to be the master and ADC2 the slave of the injected
 * simultaneous conversion configured by CubeMX.
 * Call before enabling motor PWM, with zero phase current. Blocks until
 * 64 VREFINT readings and 1000 zero-current sample sets are captured
 * (or timeout). ADC1 regular rank 1 must be VREFINT with adequate sampling
 * time. Raw samples remain packed in RAM; CSV converts them to amperes
 * using the per-rank offset and startup VREF+ calibration.
 */
HAL_StatusTypeDef CurrentSense_Init(ADC_HandleTypeDef *master_adc,
                                    ADC_HandleTypeDef *slave_adc,
                                    OPAMP_HandleTypeDef *u_opamp,
                                    OPAMP_HandleTypeDef *v_opamp,
                                    OPAMP_HandleTypeDef *w_opamp,
                                    TIM_HandleTypeDef *trigger_timer);

/**
 * @brief Handle the "adc" serial command.
 * @return true if the command belongs to this module, otherwise false.
 */
bool CurrentSense_ProcessCommand(const char *command);

/** @brief Stop a completed acquisition and transmit its CSV data. */
void CurrentSense_Task(void);

/** @brief Return true while acquisition or CSV transmission is in progress. */
bool CurrentSense_IsBusy(void);

#ifdef __cplusplus
}
#endif

#endif /* CURRENT_SENSE_H */
