#ifndef MOTOR_CONTROL_CONFIG_H
#define MOTOR_CONTROL_CONFIG_H

/* Motor and open-loop six-step parameters. */
#define MOTOR_CONTROL_POLE_PAIRS                    7U

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

#endif /* MOTOR_CONTROL_CONFIG_H */
