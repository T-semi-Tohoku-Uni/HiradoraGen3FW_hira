"""実ARM ELFの電流目標APIを検証。PWMや実機への接続は行わない。"""
import struct
import sys

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_PRIMASK, UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_LR
from test_current_pi import Firmware, close


def test(path):
    f = Firmware(path)
    calibration_active = False
    inject_fault = False

    def calibration_stub(uc, address, size, data):
        # 入力検査と目標確定の間に保護ISRがfaultをラッチした状況を再現する。
        if inject_fault:
            uc.mem_write(f.foc['fault'], struct.pack('<I', f.TEXT))
        uc.reg_write(UC_ARM_REG_R0, int(calibration_active))
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    address = f.symbols['MotorCalibration_IsActive'] & ~1
    f.uc.hook_add(UC_HOOK_CODE, calibration_stub, begin=address, end=address)

    def set_state(name, value, fmt='<I'):
        f.uc.mem_write(f.foc[name], struct.pack(fmt, value))

    def target(d, q, expected=True):
        assert bool(f.call('Foc_SetCurrentTarget', floats=(d, q))) == expected

    def values(d, q):
        close(f.get('current_target_d', '<f'), d)
        close(f.get('current_target_q', '<f'), q)

    target(0, 9)
    values(0, 9)
    assert not f.call('FocVoltage_IsActive')
    assert not f.call('FocVoltage_IsRunning')
    # 9 A command limit is on vector magnitude, below the 10 A phase trip.
    target(5.4, 7.2)
    for d, q in ((0, 9.01), (0, -9.01), (7, 7), (float('nan'), 0),
                 (0, float('inf')), (float('-inf'), 0), (1e30, 1e30)):
        target(d, q, False)
        values(5.4, 7.2)
    calibration_active = True
    target(0, 0, False)
    values(5.4, 7.2)
    calibration_active = False

    set_state('active', True, '<?')
    set_state('control_mode', 0)
    target(0, 0, False)
    assert f.get('control_mode', '<I') == 0
    values(5.4, 7.2)
    set_state('control_mode', 1)
    # 運転中の更新でランプ現在値や積分器をリセットしない。
    set_state('current_ref_d', 0.25, '<f')
    set_state('current_ref_q', 0.5, '<f')
    set_state('current_pi', 0.75, '<f')
    for q in (0, -9, 9):
        target(0, q)
        values(0, q)
        close(f.get('current_ref_d', '<f'), 0.25)
        close(f.get('current_ref_q', '<f'), 0.5)
        close(f.get('current_pi', '<f'), 0.75)
        assert f.call('FocVoltage_IsActive')
    # 呼出前の割り込み禁止状態を成功・拒否の両方で保持する。
    for mask in (0, 1):
        f.uc.reg_write(UC_ARM_REG_PRIMASK, mask)
        target(0, -1)
        assert f.uc.reg_read(UC_ARM_REG_PRIMASK) == mask
        inject_fault = True
        target(0, 1, False)
        values(0, -1)
        assert f.uc.reg_read(UC_ARM_REG_PRIMASK) == mask
        inject_fault = False
        target(0, 0, False)
        set_state('fault', 0)
    f.uc.reg_write(UC_ARM_REG_PRIMASK, 0)
    # 停止後の準備では既存UART同様にランプと積分器をゼロに戻す。
    set_state('active', False, '<?')
    target(0, 0)
    close(f.get('current_ref_q', '<f'), 0)
    assert not any(f.uc.mem_read(f.foc['current_pi'], 28))
    assert not f.messages  # API自身はprintfも文字列コマンド処理も呼ばない。
    print(f'PASS: current target limits, lifecycle, fault race, PRIMASK, PI retention: {path}')


if __name__ == '__main__':
    for path in sys.argv[1:]:
        test(path)
