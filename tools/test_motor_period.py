"""Check compiled center-aligned period and voltage compares without hardware."""
import struct
import sys

from test_current_pi import Firmware
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_S0,
)


def test(path):
    f = Firmware(path)
    f.uc.mem_map(0x40000000, 0x100000)

    def clock(uc, address, size, data):
        if address == (f.symbols['HAL_RCC_GetPCLK2Freq'] & ~1):
            uc.reg_write(UC_ARM_REG_R0, 160000000)
        else:
            uc.mem_write(uc.reg_read(UC_ARM_REG_R0), bytes(20))  # RCC dividers = 1
            uc.mem_write(uc.reg_read(UC_ARM_REG_R1), bytes(4))
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    for name in ('HAL_RCC_GetPCLK2Freq', 'HAL_RCC_GetClockConfig'):
        address = f.symbols[name] & ~1
        f.uc.hook_add(UC_HOOK_CODE, clock, begin=address, end=address)

    def put(address, value):
        f.uc.mem_write(address, struct.pack('<I', value))

    put(f.symbols['htim1'], 0x40012C00)
    put(0x40012C28, 0)  # TIM1 PSC
    put(0x40012C2C, 4000)  # TIM1 ARR
    # Stop is stubbed by Firmware; the I2C handle is not dereferenced.
    assert f.call('MotorControl_Init', f.symbols['htim1'], f.CONFIG) == 0
    f.call('MotorControl_GetPeriodSeconds')
    dt = struct.unpack('<f', struct.pack('<I', f.uc.reg_read(UC_ARM_REG_S0)))[0]
    assert abs(dt - 50e-6) < 1e-10, dt
    f.uc.mem_write(f.symbols['motor_mode'], bytes([2]))  # voltage mode
    for vd, expected in ((0, (2000, 2000, 2000)), (1, (2125, 1875, 1875))):
        assert f.call('MotorControl_SetVoltage', floats=(0, vd, 0, 24)) == 1
        actual = struct.unpack('<3I', f.uc.mem_read(f.symbols['voltage_compare'], 12))
        assert actual == expected, (actual, expected)
        assert f.call('MotorControl_SetVoltageSinCos', floats=(0, 1, vd, 0, 24)) == 1
        actual = struct.unpack('<3I', f.uc.mem_read(f.symbols['voltage_compare'], 12))
        assert actual == expected, (actual, expected)
    put(0x40012C2C, 0)
    assert f.call('MotorControl_Init', f.symbols['htim1'], f.CONFIG) == 1
    print(f'PASS: 50 us, zero/nonzero voltage compares, ARR=0 rejection: {path}')


if __name__ == '__main__':
    for path in sys.argv[1:]:
        test(path)
