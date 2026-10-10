"""Exercise the actual FOC ADC path in ARM ELF, with synthetic sensor/current inputs.

python tools/test_encoder_h2.py build/Debug/HiradoraGen3FW.elf build/Release/HiradoraGen3FW.elf
Requires unicorn and pyelftools, as does test_current_pi.py.
"""
import math
import struct
import sys

from test_current_pi import Firmware
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_LR


def test(path, factory=Firmware):
    f = factory(path)
    sensor = bytes(32)

    def stub(uc, address, size, data):
        if address == (f.symbols['AS5047P_GetSample'] & ~1):
            uc.mem_write(uc.reg_read(UC_ARM_REG_R0), sensor)
            uc.reg_write(UC_ARM_REG_R0, 1)
        else:
            uc.reg_write(UC_ARM_REG_R0, 0)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    for name in ('AS5047P_GetSample', 'HAL_GetTick'):
        address = f.symbols[name] & ~1
        f.uc.hook_add(UC_HOOK_CODE, stub, begin=address, end=address)
    f.uc.mem_write(f.foc['active'], b'\x01')
    f.uc.mem_write(f.foc['running'], b'\x01')
    f.uc.mem_write(f.symbols['SystemCoreClock'], struct.pack('<I', 160000000))
    f.uc.mem_write(f.STATE, struct.pack('<4f', 1, -0.5, 1, -0.5))
    assert f.get('h2_enabled', '<?') is False
    assert f.get('h2_gain', '<i') == 1
    a2, b2 = -0.051, 0.055
    f.uc.mem_write(f.foc['calibration'] + 24, struct.pack('<2f', a2, b2))
    for direction in (-1, 1):
        offset = 0.73
        f.uc.mem_write(f.foc['calibration'] + 16, struct.pack('<if', direction, offset))
        for enabled, gain in ((False, 1), (True, 0), (True, 1), (True, -1)):
            f.command('foc h2 on' if enabled else 'foc h2 off')
            f.command(f'foc h2 gain {gain}')
            for index in range(361):
                theta = index * 2 * math.pi / 360
                theta = struct.unpack('<f', struct.pack('<f', theta))[0]
                raw_elec = (7 * theta) % (2 * math.pi)
                sensor = struct.pack('<H2xffIIIIB3x', index, theta, raw_elec, 0, 0, 0, 42, 1)
                f.call('FocVoltage_CurrentISR', f.STATE, 0)
                assert f.get('observation_valid', '<?')
                assert bytes(f.uc.mem_read(f.foc['cycle_angle'], 32)) == sensor
                base = (direction * 7 * theta - offset) % (2 * math.pi)
                # Specified electrical error is independent of calibration direction.
                delta = -gain * (a2*math.cos(2*theta) + b2*math.sin(2*theta)) if enabled else 0
                actual_delta = f.get('observed_correction', '<f')
                actual = f.get('observed_electrical', '<f')
                expected = (base + delta) % (2 * math.pi)
                assert abs(actual_delta - delta) < 2e-6
                assert abs(math.remainder(actual - expected, 2 * math.pi)) < 8e-6
                assert 0 <= actual < 2 * math.pi
                # The same corrected angle must drive Park, not just telemetry.
                assert abs(f.get('measured_d', '<f') + math.cos(expected)) < 1e-5
                assert abs(f.get('measured_q', '<f') - direction * math.sin(expected)) < 1e-5
                if not enabled or gain == 0:
                    assert actual == f.get('observed_base_electrical', '<f')
                    assert actual_delta == 0
    for invalid in ('2', '-2', 'nan', '1 trailing'):
        f.command('foc h2 gain ' + invalid)
        assert f.get('h2_gain', '<i') == -1
    f.call('FocVoltage_GetObservationISR', f.CONFIG)
    values = struct.unpack('<7f', f.uc.mem_read(f.CONFIG, 28))
    assert values[2] == f.get('observed_electrical', '<f')
    assert values[3:5] == struct.unpack('<2f', sensor[4:12])
    assert values[5] == f.get('observed_base_electrical', '<f')
    assert values[6] == f.get('observed_correction', '<f')
    print(f'PASS: H2 on/off, gain, direction, phase, wrap, raw preservation, Park and observation: {path}')


if __name__ == '__main__':
    for path in sys.argv[1:]:
        test(path)
