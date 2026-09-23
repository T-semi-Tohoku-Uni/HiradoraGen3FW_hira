#ifndef MOTOR_CONTROL_CONFIG_H
#define MOTOR_CONTROL_CONFIG_H

/* Motor and open-loop six-step parameters. */
#define MOTOR_CONTROL_POLE_PAIRS                    7U

/* 角度通信の締切と鮮度。TIM1停止中はmainから低頻度取得する。 */
#define MOTOR_CONTROL_ENCODER_TIMEOUT_US            100U
#define MOTOR_CONTROL_ENCODER_STALE_MS              10U
#define MOTOR_CONTROL_ENCODER_DIAG_PERIOD_MS        20U
#define MOTOR_CONTROL_ENCODER_DIAG_STALE_MS         100U

/* モーター交換・相配線変更時はIDを変更し、必ず手動で再校正する。 */
#define MOTOR_CONTROL_MOTOR_ID                     1U
#define MOTOR_CONTROL_KV_RPM_PER_VOLT               140.0f
/* 定格不明のため、以下は実験用保護値。KVから許容電流は推定しない。 */
#define MOTOR_CONTROL_CURRENT_LIMIT_A              5.0f
#define MOTOR_CONTROL_CAL_CURRENT_LIMIT_A          5.0f
/* N5065実測：1Vへのramp中、約0.70Vで5Aに到達。0.4Vで校正完走を確認。 */
#define MOTOR_CONTROL_CAL_VOLTAGE                   0.4f
#define MOTOR_CONTROL_VM_MIN_VOLTS                 6.0f
#define MOTOR_CONTROL_VOLTAGE_LIMIT                 3.0f
#define MOTOR_CONTROL_PWM_MARGIN                   0.05f
#define MOTOR_CONTROL_CAL_RAMP_MS                  500U
#define MOTOR_CONTROL_CAL_HOLD_MS                  700U
#define MOTOR_CONTROL_CAL_SWEEP_MS                 4000U
#define MOTOR_CONTROL_CAL_MOTION_TOLERANCE          0.20f
#define MOTOR_CONTROL_CAL_POSITION_TOLERANCE_RAD    0.15f
#define MOTOR_CONTROL_CAL_WATCHDOG_MS               20U

/* Charge all three bootstrap capacitors before each PWM start. */
#define MOTOR_CONTROL_BOOTSTRAP_CHARGE_US           750U

/*
 * Six-step duty profile. Duty values use 0.1 % units (50 = 5.0 %).
 *
 * During the speed ramp, duty starts at RUN_START_DUTY_X10 at START_RPM,
 * then rises by DUTY_RISE_X10 every DUTY_RISE_RPM. The result is limited
 * to MAX_DUTY_X10. With the values below duty is 5.0 % at 60 rpm,
 * 5.7 % at 120 rpm, and 7.0 % at 240 rpm.
 */
#define MOTOR_CONTROL_ALIGNMENT_DUTY_X10            50U
#define MOTOR_CONTROL_RUN_START_DUTY_X10            50U
#define MOTOR_CONTROL_DUTY_RISE_X10                 20U
#define MOTOR_CONTROL_DUTY_RISE_RPM                 180U
#define MOTOR_CONTROL_MAX_DUTY_X10                  200U

/* A fixed vector is required before the rotor can follow the commutation. */
#define MOTOR_CONTROL_ALIGNMENT_TIME_MS             200U

/* Ramp from START_RPM to the commanded RPM after alignment. */
#define MOTOR_CONTROL_ACCELERATION_TIME_MS          6000U
#define MOTOR_CONTROL_START_RPM                     60U

/* Accepted range for the serial "run cw/ccw <rpm>" command. */
#define MOTOR_CONTROL_MIN_TARGET_RPM                60U
#define MOTOR_CONTROL_MAX_TARGET_RPM                3000U

/* Maximum permitted deviation from 50% in the legacy manual PWM mode. */
#define MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT       10.0f

/* VM（PC2）の分圧と監視設定。校正中は上下限で停止する。
 * 既存manual/six-stepには自動停止を接続していない。 */
#define MOTOR_CONTROL_VM_NOMINAL_VOLTS              24.0f
#define MOTOR_CONTROL_VM_MAX_VOLTS                  30.0f
#define MOTOR_CONTROL_VM_DIVIDER_TOP_OHMS           100000.0f
#define MOTOR_CONTROL_VM_DIVIDER_BOTTOM_OHMS        10000.0f
#define MOTOR_CONTROL_VM_SAMPLE_PERIOD_MS           1U
#define MOTOR_CONTROL_VM_TIMEOUT_MS                 10U
#define MOTOR_CONTROL_VM_STALE_MS                   100U

#endif /* MOTOR_CONTROL_CONFIG_H */
