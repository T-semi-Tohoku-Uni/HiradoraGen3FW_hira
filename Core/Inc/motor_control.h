#ifndef MOTOR_CONTROL_H
#define MOTOR_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"
#include "motor_control_config.h"

#include <stdbool.h>

typedef enum
{
  MOTOR_CONTROL_PHASE_U = 0,
  MOTOR_CONTROL_PHASE_V,
  MOTOR_CONTROL_PHASE_W
} MotorControlPhase;

/**
 * @brief Initialize motor control with all PWM outputs stopped.
 * @param htim Advanced-control timer with CH1/CH1N through CH3/CH3N.
 * @param hi2c Gate-driver I2C handle. Failed starts leave PWM stopped.
 */
HAL_StatusTypeDef MotorControl_Init(TIM_HandleTypeDef *htim,
                                     I2C_HandleTypeDef *hi2c);

/**
 * @brief Parse and apply one serial command.
 *
 * Supported commands:
 *   <offset>    Apply an offset in percent to the currently selected phase.
 *   u <offset>  Select U and apply the offset. V and W return to 50%.
 *   v <offset>  Select V and apply the offset. U and W return to 50%.
 *   w <offset>  Select W and apply the offset. U and V return to 50%.
 *   mid         Return all phases to 50%.
 *   stop        Disable all TIM1 PWM outputs.
 *   start       Restart all phases at 50%.
 *   run cw <rpm>    Start clockwise open-loop six-step drive.
 *   run ccw <rpm>   Start counter-clockwise open-loop six-step drive.
 *   status      Print the current state.
 *
 * @return true when the command was valid, otherwise false.
 */
bool MotorControl_ProcessCommand(const char *command);

/**
 * @brief Process only the emergency "stop" command.
 * @return true when a stop command was handled.
 */
bool MotorControl_ProcessStopCommand(const char *command);

/** @brief Immediately disable all main and complementary PWM outputs. */
void MotorControl_Stop(void);

/**
 * @brief Return the active six-step sector (1 through 6), or 0 otherwise.
 *
 * The value is updated atomically with each commutation and can be sampled
 * from the ADC interrupt.
 */
uint8_t MotorControl_GetSector(void);
/* CEN=0かつ全モーター出力OFF専用。FOCは頂点下降、それ以外は底上昇で再始動。 */
HAL_StatusTypeDef MotorControl_ResetTimerPhase(TIM_HandleTypeDef *timer);
/* 校正/FOC共通の電圧モード。RCR=1必須。3相CCRはFOCで次の頂点、校正で次の底に反映。 */
void MotorControl_FocAdcISR(void);
void MotorControl_FocAdcBeginISR(void);
void MotorControl_PrintFocPhase(void);
bool MotorControl_IsStopped(void);
bool MotorControl_IsVoltageMode(void);
HAL_StatusTypeDef MotorControl_StartVoltage(void);
bool MotorControl_SetVoltage(float angle, float vd, float vq, float vm);


#ifdef __cplusplus
}
#endif

#endif /* MOTOR_CONTROL_H */
