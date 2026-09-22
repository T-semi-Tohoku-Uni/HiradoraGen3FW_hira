#ifndef MOTOR_CONTROL_CONFIG_H
#define MOTOR_CONTROL_CONFIG_H

/* Motor and open-loop six-step parameters. */
#define MOTOR_CONTROL_POLE_PAIRS                    7U

/* 角度通信の締切と鮮度。TIM1停止中はmainから低頻度取得する。 */
#define MOTOR_CONTROL_ENCODER_TIMEOUT_US            100U
#define MOTOR_CONTROL_ENCODER_STALE_MS              10U
#define MOTOR_CONTROL_ENCODER_DIAG_PERIOD_MS        20U
#define MOTOR_CONTROL_ENCODER_DIAG_STALE_MS         100U

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

/* VM（PC2）の分圧と監視設定。上限判定は現在は表示のみ。
 * 運転停止への接続はFOC実装時に行う。 */
#define MOTOR_CONTROL_VM_NOMINAL_VOLTS              24.0f
#define MOTOR_CONTROL_VM_MAX_VOLTS                  30.0f
#define MOTOR_CONTROL_VM_DIVIDER_TOP_OHMS           100000.0f
#define MOTOR_CONTROL_VM_DIVIDER_BOTTOM_OHMS        10000.0f
#define MOTOR_CONTROL_VM_SAMPLE_PERIOD_MS           1U
#define MOTOR_CONTROL_VM_TIMEOUT_MS                 10U
#define MOTOR_CONTROL_VM_STALE_MS                   100U

#endif /* MOTOR_CONTROL_CONFIG_H */
