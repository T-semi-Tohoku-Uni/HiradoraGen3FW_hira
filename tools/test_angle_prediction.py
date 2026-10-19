"""Execute production ARM predictor, timer timing and FOC path in Unicorn.

python tools/test_angle_prediction.py build/Release/HiradoraGen3FW.elf
No hardware is accessed. Peripheral timestamps/CORDIC are modeled.
"""
import math
import struct
import sys
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_S0
from test_cordic import CordicFirmware


def delta(a, b):
    return (a-b+math.pi) % (2*math.pi)-math.pi


def test(path):
    f = CordicFirmware(path)
    prediction_enabled=f.get('prediction_mode', '<I')==1
    def put(addr, value):
        f.uc.mem_write(addr, struct.pack('<I', value & 0xffffffff))
    def fp(addr, value):
        f.uc.mem_write(addr, struct.pack('<f', value))
    def result():
        return struct.unpack('<f', struct.pack('<I', f.uc.reg_read(UC_ARM_REG_S0)))[0]

    # Actual alpha-beta implementation: stationary, both signs, wrap, gaps,
    # quantization and acceleration. Targets include future and past samples.
    for speed in (0., 70., -70., 730., -730.):
        f.call('AnglePredictor_Reset', f.STATE, floats=(6.27,))
        angle = 6.27
        for i in range(500):
            dt = .0006 if i % 23 == 0 else .0002
            angle += speed*dt
            measurement = round((angle % (2*math.pi)) / (7*2*math.pi/4096)) * (7*2*math.pi/4096)
            assert f.call('AnglePredictor_Update', f.STATE, floats=(measurement, dt, .0005))
        for horizon in (-.00001, .000075, .00030):
            f.call('AnglePredictor_At', f.STATE, floats=(horizon,))
            assert abs(delta(result(), angle+speed*horizon)) < .012
    for args in ((math.nan,.00005,.0005), (0,0,.0005), (0,.002,.0005), (0,.00005,0)):
        assert not f.call('AnglePredictor_Update', f.STATE, floats=args)
    f.call('AnglePredictor_Reset', f.STATE, floats=(0.,))
    for i in range(1, 501):
        t=i*.00005
        angle=.5*10000*t*t
        assert f.call('AnglePredictor_Update', f.STATE, floats=(angle % (2*math.pi), .00005, .0005))
    f.call('AnglePredictor_At', f.STATE, floats=(.000125,))
    assert abs(delta(result(), .5*10000*(t+.000125)**2)) < .02

    # Timer phase -> ADC representative time and next update + half period.
    put(f.symbols['SystemCoreClock'], 160000000)
    put(f.symbols['motor_timer'], f.symbols['htim1'])
    put(f.symbols['htim1'], 0x40012c00)
    put(f.symbols['control_tick_hz'], 20000)
    put(0x40012c2c, 4000)
    put(0x40012c40, 3999)  # CCR4
    for top in (100000, 0xfffffff0):
        for phase in (300, 3900, 4100, 7199):
            put(f.symbols['foc_adc_began'], top+phase)
            put(f.symbols['foc_adc_phase_cycles'], top+phase)
            put(f.symbols['foc_adc_phase'], phase)
            put(0xe0001004, top+phase+100)
            assert f.call('MotorControl_GetFocTimesISR', f.CONFIG, f.CONFIG+4)
            adc, pwm = struct.unpack('<2I', f.uc.mem_read(f.CONFIG, 8))
            assert adc == (top+101) & 0xffffffff
            assert pwm == (top+12000) & 0xffffffff
    put(f.symbols['foc_adc_phase'], 7300)
    assert not f.call('MotorControl_GetFocTimesISR', f.CONFIG, f.CONFIG+4)
    put(f.symbols['foc_adc_phase'], 300)
    put(0xe0001004, 0xfffffff0+7199+8000)
    assert not f.call('MotorControl_GetFocTimesISR', f.CONFIG, f.CONFIG+4)

    # Real CurrentISR -> PI/voltage -> inverse Park. Only sensor and output
    # boundaries stubbed; actual timing helper/predictor/CORDIC remain active.
    sensor = b''
    output = []
    def stub(uc, address, size, name):
        if name == 'AS5600_GetSample':
            uc.mem_write(uc.reg_read(UC_ARM_REG_R0), sensor)
        if name == 'MotorControl_SetVoltageSinCos':
            output.append(tuple(struct.unpack('<f', struct.pack('<I', uc.reg_read(UC_ARM_REG_S0+i)))[0] for i in range(2)))
        uc.reg_write(UC_ARM_REG_R0, 0 if name == 'HAL_GetTick' else 1)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    for name in ('AS5600_GetSample', 'AS5600_Tick', 'HAL_GetTick', 'MotorControl_IsVoltageMode',
                 'HAL_GPIO_ReadPin', 'MotorControl_SetVoltageSinCos'):
        address=f.symbols[name] & ~1
        f.uc.hook_add(UC_HOOK_CODE, stub, user_data=name, begin=address, end=address)
    f.uc.mem_write(f.STATE, struct.pack('<4f', 1., -.5, 1., -.5))
    fp(f.foc['control_period'], .00005)
    for direction in (1, -1):
        for h2 in (False, True):
            put(f.foc['fault'], 0)
            f.uc.mem_write(f.foc['active'], b'\1')
            f.uc.mem_write(f.foc['running'], b'\1')
            f.uc.mem_write(f.foc['predictor_valid'], b'\0')
            f.uc.mem_write(f.foc['h2_enabled'], bytes([h2]))
            f.uc.mem_write(f.foc['calibration']+16, struct.pack('<ifff', direction, .73, -.051, .055))
            fp(f.foc['rpm'], 0.)
            put(f.foc['last_tick_cycles'], 0)
            put(f.foc['angle_sequence'], 1)
            put(f.foc['angle_cycles'], 0xffff0000)
            fp(f.foc['previous_angle'], .5)
            for i in range(1, 151):
                ref=(0xffff0000+i*32000) & 0xffffffff
                top=(ref+6000) & 0xffffffff
                mech=.5+i*.004
                sensor=struct.pack('<H2xffIIIIB3xI', 100, mech, mech*7, (ref-600)&0xffffffff,
                                   (ref+1000)&0xffffffff, 0, i, 1, ref)
                put(f.symbols['foc_adc_began'], top+300)
                put(f.symbols['foc_adc_phase_cycles'], top+300)
                put(f.symbols['foc_adc_phase'], 300)
                put(0xe0001004, top+400)
                f.call('FocVoltage_CurrentISR', f.STATE, 0)
                assert f.get('observation_valid', '<?'), f.string(f.get('fault','<I'))
                park=f.get('observed_electrical','<f')
                pwm=f.get('pwm_electrical','<f')
                velocity=struct.unpack('<2f', f.uc.mem_read(f.foc['angle_predictor'],8))[1]
                expected_advance=velocity*(12000-101)/160e6 if prediction_enabled else 0.
                assert abs(delta(pwm,park)-expected_advance) < 2e-6
                f.call('FocVoltage_TickISR')
                assert f.get('running','<?'), f.string(f.get('fault','<I'))
                assert abs(output[-1][0]-math.sin(pwm)) < 2e-6
                assert abs(output[-1][1]-math.cos(pwm)) < 2e-6
            assert velocity*direction > 100
            # Reusing a sample does not update the estimator a second time.
            before=bytes(f.uc.mem_read(f.foc['angle_predictor'],8))
            f.call('FocVoltage_CurrentISR', f.STATE, 0)
            assert bytes(f.uc.mem_read(f.foc['angle_predictor'],8)) == before
    # Age protection remains based on actual acquisition, never predicted time.
    count=len(output)
    put(0xe0001004, ref+130000)
    f.call('FocVoltage_CurrentISR', f.STATE, 0)
    f.call('FocVoltage_TickISR')
    assert not f.get('running','<?') and len(output)==count
    print('PASS: ARM predictor, +/- speed, gaps, quantization, acceleration, timer/DWT wrap, '
          'next PWM midpoint, Park/output separation, H2 +/- direction, duplicate/stale samples:', path)


if __name__ == '__main__':
    for path in sys.argv[1:]:
        test(path)
