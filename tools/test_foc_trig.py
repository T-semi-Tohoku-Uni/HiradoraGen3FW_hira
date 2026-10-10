"""ARM ELF regression for shared FOC sin/cos. Pass before and after ELF paths."""
import math
import struct
import sys

from test_current_pi import Firmware
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_S0


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def pair(f, name, angle):
    f.call(name, f.CONFIG, f.CONFIG + 4, (angle,))
    return struct.unpack('<2f', f.uc.mem_read(f.CONFIG, 8))


def test(before, after):
    old, new = Firmware(before), Firmware(after)
    two_pi = f32(2 * math.pi)
    angles = [f32(i * two_pi / 1024) for i in range(1025)]
    angles += [0., -0., f32(-1e-8), f32(two_pi - 5e-7), two_pi]
    maximum = 0.
    for angle in angles:
        # Same normalisation as the existing FOC caller, then old second wrap.
        old.call('VoltageVector_Wrap', floats=(angle,))
        wrapped = struct.unpack('<f', struct.pack('<I', old.uc.reg_read(UC_ARM_REG_S0)))[0]
        s, c = pair(new, 'VoltageVector_SinCosWrapped', wrapped)
        assert (s, c) == pair(old, 'VoltageVector_SinCos', wrapped)
        for vd, vq, vm in ((.3, -.2, 24), (0, 0, 24), (100, -100, 6)):
            assert old.call('VoltageVector_Compute', old.TEXT, floats=(wrapped, vd, vq, vm, 3, .05))
            assert new.call('VoltageVector_ComputeSinCos', new.TEXT, floats=(s, c, vd, vq, vm, 3, .05))
            a = struct.unpack('<3f', old.uc.mem_read(old.TEXT, 12))
            b = struct.unpack('<3f', new.uc.mem_read(new.TEXT, 12))
            maximum = max(maximum, *(abs(x-y) for x, y in zip(a, b)))
            assert all(abs(x-y) < 2e-7 for x, y in zip(a, b)), (angle, a, b)
    for floats in ((math.nan, 1, 0, 0, 24, 3, .05), (0, math.inf, 0, 0, 24, 3, .05),
                   (0, 1, math.nan, 0, 24, 3, .05), (0, 1, 0, 0, 0, 3, .05),
                   (0, 1, 0, 0, 24, 3, .5)):
        assert not new.call('VoltageVector_ComputeSinCos', new.TEXT, floats=floats)
    assert not new.call('VoltageVector_ComputeSinCos', 0, floats=(0, 1, 0, 0, 24, 3, .05))

    # Real CurrentISR -> TickISR; mock only sensors/peripherals and output boundary.
    f = Firmware(after)
    output, trig_calls = [], []
    sensor = struct.pack('<H2xffIIIIB3x', 100, .5, 3.5, 0, 0, 0, 42, 1)
    def stub(uc, address, size, data):
        name = data
        if name == 'AS5047P_GetSample':
            uc.mem_write(uc.reg_read(UC_ARM_REG_R0), sensor)
        if name == 'MotorControl_SetVoltageSinCos':
            output.append(tuple(uc.reg_read(UC_ARM_REG_S0+i) for i in range(2)))
        uc.reg_write(UC_ARM_REG_R0, 0 if name == 'HAL_GetTick' else 1)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    for name in ('AS5047P_GetSample', 'HAL_GetTick', 'MotorControl_IsVoltageMode',
                 'HAL_GPIO_ReadPin', 'MotorControl_SetVoltageSinCos'):
        addr = f.symbols[name] & ~1
        f.uc.hook_add(UC_HOOK_CODE, stub, user_data=name, begin=addr, end=addr)
    addr = f.symbols['arm_sin_cos_f32'] & ~1
    f.uc.hook_add(UC_HOOK_CODE, lambda *args: trig_calls.append(1), begin=addr, end=addr)
    f.uc.mem_write(f.foc['active'], b'\1')
    f.uc.mem_write(f.foc['running'], b'\1')
    f.uc.mem_write(f.symbols['SystemCoreClock'], struct.pack('<I', 160000000))
    f.uc.mem_write(f.foc['calibration'] + 16, struct.pack('<ifff', 1, .73, -.051, .055))
    f.uc.mem_write(f.STATE, struct.pack('<4f', 1, -.5, 1, -.5))
    for enabled in (False, True):
        f.command('foc h2 on' if enabled else 'foc h2 off')
        trig_calls.clear()
        f.call('FocVoltage_CurrentISR', f.STATE, 0)
        expected = tuple(struct.unpack('<I', f.uc.mem_read(f.foc[name], 4))[0]
                         for name in ('observed_sin', 'observed_cos'))
        f.call('FocVoltage_TickISR')
        assert output[-1] == expected
        assert len(trig_calls) == (2 if enabled else 1), len(trig_calls)
    # Invalid observation must never publish a cached pair from the previous cycle.
    count = len(output)
    f.uc.mem_write(f.foc['observation_valid'], b'\0')
    f.call('FocVoltage_TickISR')
    assert len(output) == count and not f.get('running', '<?')
    f.uc.mem_write(f.foc['running'], b'\1')
    f.uc.mem_write(f.foc['fault'], bytes(4))
    f.command('foc h2 off')
    sensor = struct.pack('<H2xffIIIIB3x', 100, math.nan, 3.5, 0, 0, 0, 43, 1)
    f.call('FocVoltage_CurrentISR', f.STATE, 0)
    assert not f.get('running', '<?') and not f.get('observation_valid', '<?')
    print(f'PASS: boundary/duty equivalence (max error={maximum}), invalid inputs, '
          f'same-cycle pair, sincos counts H2 OFF=1/ON=2, stale/NaN rejection: {after}')


if __name__ == '__main__':
    for after in sys.argv[2:]:
        test(sys.argv[1], after)
