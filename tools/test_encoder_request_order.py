"""Run real CurrentISR and ADC tail: next SPI request follows valid snapshot once."""
import struct
import sys
from test_current_pi import Firmware
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_LR


def test(path):
    f = Firmware(path)
    f.uc.mem_map(0x40000000, 0x100000)
    def put(addr, value):
        f.uc.mem_write(addr, struct.pack('<I', value))
    put(f.symbols['motor_timer'], f.symbols['htim1'])
    put(f.symbols['htim1'], 0x40012c00)
    put(0x40012c00, 16)  # descending
    put(0x40012c24, 3200)
    put(0x40012c2c, 4000)
    f.uc.mem_write(f.symbols['motor_mode'], b'\2')
    put(f.symbols['SystemCoreClock'], 160000000)
    put(f.symbols['control_tick_hz'], 20000)
    f.uc.mem_write(f.foc['active'], b'\1')
    f.uc.mem_write(f.foc['running'], b'\1')
    f.uc.mem_write(f.foc['calibration']+16, struct.pack('<ifff',1,.73,0,0))
    f.uc.mem_write(f.STATE, struct.pack('<4f',1,-.5,1,-.5))
    sensor = struct.pack('<H2xffIIIIB3x',100,.5,3.5,0,0,0,42,1)
    valid = True
    events = []
    def hook(uc, addr, size, name):
        nonlocal sensor
        if name == 'VoltageVector_SinCosWrapped':
            events.append('park'); return
        events.append(name)
        result = 0
        if name == 'AS5047P_GetSample':
            uc.mem_write(uc.reg_read(UC_ARM_REG_R0), sensor)
            result = int(valid)
        elif name == 'AS5047P_Tick':
            # Simulate newly published sensor data: existing FOC copy must not change.
            sensor = struct.pack('<H2xffIIIIB3x',200,1.,.7168,0,0,0,43,1)
        uc.reg_write(UC_ARM_REG_R0, result)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    for name in ('AS5047P_GetSample','AS5047P_Tick','HAL_GetTick',
                 'FocVoltage_TickISR','VoltageVector_SinCosWrapped'):
        addr=f.symbols[name]&~1
        f.uc.hook_add(UC_HOOK_CODE,hook,user_data=name,begin=addr,end=addr)
    original = sensor
    f.call('FocVoltage_CurrentISR', f.STATE, 0)
    assert bytes(f.uc.mem_read(f.foc['cycle_angle'],32)) == original
    f.call('MotorControl_FocAdcISR')
    assert events.count('AS5047P_Tick') == 1, events
    assert events.index('AS5047P_GetSample') < events.index('AS5047P_Tick') < events.index('park') < events.index('FocVoltage_TickISR')
    # Startup still acquires at the tail when no running snapshot is available.
    events.clear()
    f.uc.mem_write(f.foc['running'],b'\0')
    f.call('FocVoltage_CurrentISR', f.STATE, 0)
    assert 'AS5047P_Tick' not in events
    f.call('MotorControl_FocAdcISR')
    assert events.count('AS5047P_Tick') == 1
    assert events.index('FocVoltage_TickISR') < events.index('AS5047P_Tick')
    for failure in ('invalid','stale'):
        events.clear()
        put(f.foc['fault'],0)
        f.uc.mem_write(f.foc['running'],b'\1')
        valid = failure != 'invalid'
        if failure == 'stale':
            sensor=struct.pack('<H2xffIIIIB3x',100,.5,3.5,0xfe000000,0,0,44,1)
        f.call('FocVoltage_CurrentISR',f.STATE,0)
        assert 'AS5047P_Tick' not in events and not f.get('running','<?')
    print(f'PASS: snapshot/request/Park order, no duplicate tail request, arming fallback, invalid/stale guards: {path}')


if __name__=='__main__':
    for path in sys.argv[1:]: test(path)
