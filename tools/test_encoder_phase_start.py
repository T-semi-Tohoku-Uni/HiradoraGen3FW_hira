"""Exercise compiled PWM start gates with simulated phase/clock/fault inputs."""
import struct
import sys
from test_current_pi import Firmware
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_PRIMASK


def test(path):
    for case in ('ok', 'timeout', 'encoder_lost', 'fault_before', 'fault_race',
                 'late_after_enable', 'wrap_after_enable', 'retry', 'manual'):
        f = Firmware(path)
        f.uc.mem_map(0x40000000, 0x100000)
        tim = 0x40012C00
        def put(a, n): f.uc.mem_write(a, struct.pack('<I', n))
        def get(a): return struct.unpack('<I', f.uc.mem_read(a, 4))[0]
        put(f.symbols['motor_timer'], f.symbols['htim1'])
        put(f.symbols['htim1'], tim)
        put(tim + 0x2C, 4000)
        put(f.symbols['SystemCoreClock'], 160000000)
        calls = dict(phase=0, fault=0)
        def tick(uc, address, size, data):
            put(0xE0001004, get(0xE0001004)+10)
        f.uc.hook_add(UC_HOOK_CODE, tick)
        def stub(uc, address, size, name):
            result = 0
            if name == 'HAL_RCC_GetPCLK2Freq': result = 160000000
            elif name == 'HAL_GPIO_ReadPin': result = 1
            elif name == 'FocVoltage_IsActive': result = int(case != 'manual')
            elif name == 'FocVoltage_CanEnablePwm':
                calls['fault'] += 1
                result = int(case != 'fault_before' and not (case == 'fault_race' and calls['fault'] == 2))
            elif name == 'AS5047P_GetTimerPhase':
                calls['phase'] += 1
                seq = {'late_after_enable': [1150,1150,1300],
                       'wrap_after_enable': [1150,1150,0],
                       'retry': [1150,1250,1150,1150,1200]}.get(case, [1150,1150,1200])
                phase = 0 if case == 'timeout' else seq[min(calls['phase']-1, len(seq)-1)]
                put(uc.reg_read(UC_ARM_REG_R0), phase)
                put(0x40013424, seq[min(calls['phase'], len(seq)-1)])  # TIM8 CNT after start
                result = int(case != 'encoder_lost')
            uc.reg_write(UC_ARM_REG_R0, result)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        for name in ('MotorControl_PreparePwmStart', 'MotorControl_ResetTimerPhase',
                     'HAL_RCC_GetPCLK2Freq', 'HAL_GPIO_ReadPin', 'FocVoltage_IsActive',
                     'FocVoltage_CanEnablePwm', 'AS5047P_GetTimerPhase'):
            a = f.symbols[name] & ~1
            f.uc.hook_add(UC_HOOK_CODE, stub, user_data=name, begin=a, end=a)
        result = f.call('MotorControl_StartAtMidpoint')
        success = case in ('ok', 'retry', 'manual')
        assert (result == 0) == success, (case,result)
        assert bool(get(tim) & 1) == success, case
        assert bool(get(tim+0x44) & 0x8000) == success, case
        assert bool(get(tim+0x20) & 0x555) == success, case
        assert f.uc.reg_read(UC_ARM_REG_PRIMASK) == 0, case
        if case == 'manual': assert calls['phase'] == 0
        print('PASS', case, path)


if __name__ == '__main__':
    for path in sys.argv[1:]: test(path)
