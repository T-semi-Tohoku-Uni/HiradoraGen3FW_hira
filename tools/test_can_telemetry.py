"""実ELFで停止・正逆転・角度周回・時刻周回・欠測後の再同期を検証。"""
import struct
import sys
from test_current_pi import Firmware
from unicorn.arm_const import UC_ARM_REG_R2, UC_ARM_REG_R3


def test(path):
    f = Firmware(path)
    def reset():
        f.uc.mem_write(f.STATE, bytes(20))
    def step(raw, cycles):
        f.uc.reg_write(UC_ARM_REG_R2, cycles & 0xFFFFFFFF)
        f.uc.reg_write(UC_ARM_REG_R3, 160000000)
        f.call('CanTelemetry_Update', f.STATE, raw)
        return struct.unpack('<IIiHHhBB', f.uc.mem_read(f.STATE, 20))
    reset()
    assert step(16383, 100)[4:] == (8191, 0, 1, 0)
    assert step(16383, 1600100)[4:] == (8191, 0, 1, 1)
    for sign in (1, -1):
        reset()
        raw = 16300 if sign == 1 else 20
        cycles = 0xFFFF0000
        step(raw, cycles)
        for i in range(1, 6):
            result = step((raw + sign * 164 * i) % 16384, cycles + i * 320000)
        assert result[5] == sign * 300, result
        assert result[7] == 1
        result = step(100, cycles + 4000000)
        assert result[5] == 0 and result[7] == 0
        result = step(100, cycles + 5600000)
        assert result[5] == 0 and result[7] == 1
    assert step(20000, 0) == (0,) * 8
    print('PASS CAN telemetry:', path)


if __name__ == '__main__':
    for path in sys.argv[1:]:
        test(path)
