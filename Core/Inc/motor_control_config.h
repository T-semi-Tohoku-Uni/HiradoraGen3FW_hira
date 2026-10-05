#ifndef MOTOR_CONTROL_CONFIG_H
#define MOTOR_CONTROL_CONFIG_H

/* Motor and open-loop six-step parameters. */
#define MOTOR_CONTROL_POLE_PAIRS                    7U

/* FOC electrical error: 0.0750*cos(2*theta_m - 2.327) [rad_elec].
 * Already in the FOC frame: do not multiply by pole pairs or direction.
 * gain=+1 subtracts this error; -1 reverses it. Runtime default is OFF. */
#define MOTOR_CONTROL_ENCODER_H2_AMPLITUDE_ELEC_RAD  0.0750f
#define MOTOR_CONTROL_ENCODER_H2_PHASE_RAD           -2.327f

/* 角度通信の締切と鮮度。TIM1停止中はmainから低頻度取得する。 */
#define MOTOR_CONTROL_ENCODER_TIMEOUT_US            100U
#define MOTOR_CONTROL_ENCODER_STALE_MS              10U
#define MOTOR_CONTROL_ENCODER_DIAG_PERIOD_MS        20U
#define MOTOR_CONTROL_ENCODER_DIAG_STALE_MS         100U

/* モーター交換・相配線変更時はIDを変更し、必ず手動で再校正する。 */
#define MOTOR_CONTROL_MOTOR_ID                     1U
#define MOTOR_CONTROL_KV_RPM_PER_VOLT               140.0f
/* 定格不明のため、以下は実験用保護値。KVから許容電流は推定しない。
 * CURRENT_LIMIT_AはFOC通常運転用。校正にはCAL_CURRENT_LIMIT_Aを使う。
 * いずれも電流指令値ではなく、超過時に出力を停止する閾値。 */
#define MOTOR_CONTROL_CURRENT_LIMIT_A              20.0f
#define MOTOR_CONTROL_CAL_CURRENT_LIMIT_A          15.0f
/* N5065実測：1V/10Aで7回完走、最大約8.41A。通常運転用閾値とは独立。 */
#define MOTOR_CONTROL_CAL_VOLTAGE                   1.0f
#define MOTOR_CONTROL_VM_MIN_VOLTS                 6.0f
#define MOTOR_CONTROL_VOLTAGE_LIMIT                 8.0f
#define MOTOR_CONTROL_PWM_MARGIN                   0.05f
#define MOTOR_CONTROL_CAL_RAMP_MS                  500U
#define MOTOR_CONTROL_CAL_HOLD_MS                  700U
#define MOTOR_CONTROL_CAL_SWEEP_MS                 4000U
#define MOTOR_CONTROL_CAL_MOTION_TOLERANCE          0.20f
#define MOTOR_CONTROL_CAL_POSITION_TOLERANCE_RAD    0.15f
#define MOTOR_CONTROL_CAL_WATCHDOG_MS               20U

/* 機械1回転の診断専用。既存校正値は変更せず、低電圧で長時間走査する。 */
#define MOTOR_CONTROL_CAL_MAP_VOLTAGE               0.8f
#define MOTOR_CONTROL_CAL_MAP_CURRENT_LIMIT_A       10.0f
#define MOTOR_CONTROL_CAL_MAP_POINTS                1000U
#define MOTOR_CONTROL_CAL_MAP_SWEEP_MS              (MOTOR_CONTROL_CAL_SWEEP_MS * MOTOR_CONTROL_POLE_PAIRS)
#define MOTOR_CONTROL_CAL_MAP_TRAVEL_TOLERANCE      0.05f

/* dq電圧一定FOCの実験用。通常運転の保護にはCURRENT_LIMIT_Aを使う。 */
#define MOTOR_CONTROL_FOC_MAX_VOLTS                 1.0f
#define MOTOR_CONTROL_FOC_SLEW_VOLTS_PER_SEC        1.0f
#define MOTOR_CONTROL_FOC_MAX_RPM                   600.0f
#define MOTOR_CONTROL_FOC_ANGLE_MAX_AGE_US          250U
#define MOTOR_CONTROL_FOC_MAIN_TIMEOUT_MS          20U
#define MOTOR_CONTROL_FOC_STANDSTILL_MS            300U
#define MOTOR_CONTROL_FOC_STANDSTILL_RAD           0.02f
/* 2026-09-25: ±Vq/±Vd試験でADC電流は印加電圧と逆符号だったため、
 * dq観測だけを反転する。raw相電流ログ・絶対値の過電流保護は変更しない。
 * 電流の絶対精度、サンプル時刻の影響は別途検証が必要。 */
#define MOTOR_CONTROL_FOC_CURRENT_POLARITY         -1.0f

/* 電流PI。2026-10-05: 現在の負荷で正方向3～14Aの短時間試験で調整。
 * Kp [V/A], Ki [V/(A*s)]。追加比較: docs/current_pi_sweep_2026-10-05.md。
 * 指令上限は相電流の過電流停止閾値とは別。積分項の単位はV。
 * CURRENT_REF_MAX_A < CURRENT_LIMIT_A が必須（同値は設定エラー）。 */
#define MOTOR_CONTROL_CURRENT_KP_D                  0.6f
#define MOTOR_CONTROL_CURRENT_KI_D                  120.0f
#define MOTOR_CONTROL_CURRENT_KP_Q                  0.6f
#define MOTOR_CONTROL_CURRENT_KI_Q                  120.0f
#define MOTOR_CONTROL_CURRENT_INTEGRAL_LIMIT_D      15.0f
#define MOTOR_CONTROL_CURRENT_INTEGRAL_LIMIT_Q      15.0f
#define MOTOR_CONTROL_CURRENT_REF_MAX_A             15.0f
#define MOTOR_CONTROL_CURRENT_SLEW_A_PER_SEC         60.0f
#define MOTOR_CONTROL_CURRENT_MAX_VOLTS             5.0f

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
