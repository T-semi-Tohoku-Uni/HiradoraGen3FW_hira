"""実起動コードのMOE書込みを監視し、I2C待ち中の許可失効を再現する。"""
import struct
import sys
from test_current_pi import Firmware
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_PC, UC_ARM_REG_LR


def run(path, cancel):
    f = Firmware(path)
    f.uc.mem_map(0x40000000, 0x100000)
    tim = 0x40012C00
    callback = f.RETURN + 0x100
    f.uc.mem_write(callback, b'\x70\x47')
    def put(addr, value): f.uc.mem_write(addr, struct.pack('<I', value))
    put(f.symbols['motor_timer'], f.symbols['htim1'])
    put(f.symbols['htim1'], tim)
    put(f.symbols['htim1'] + 20, 1)
    put(tim, 0x20)
    put(tim + 0x30, 1)
    put(tim + 0x2C, 4000)
    put(f.symbols['SystemCoreClock'], 160000000)
    enabled = []
    permitted = True
    def hook(uc, address, size, data):
        nonlocal permitted
        result = 0
        if address == callback:
            result = int(permitted)
        elif address == (f.symbols['STSPIN32G4_CheckAndClearFaults'] & ~1):
            uc.mem_write(uc.reg_read(UC_ARM_REG_R1), bytes(3))
            if cancel == 'before_bootstrap': permitted = False
        elif address == (f.symbols['STSPIN32G4_ClearFaults'] & ~1):
            if cancel == 'after_bootstrap': permitted = False
        elif address == (f.symbols['STSPIN32G4_ReadStatus'] & ~1):
            uc.mem_write(uc.reg_read(UC_ARM_REG_R1), b'\x80')
        elif address == (f.symbols['HAL_GPIO_ReadPin'] & ~1): result = 1
        elif address == (f.symbols['MotorControl_Stop'] & ~1):
            put(tim + 0x44, 0)
            put(tim, 0x20)
            put(tim + 0x20, 0)
        uc.reg_write(UC_ARM_REG_R0, result)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    for name in ('STSPIN32G4_CheckAndClearFaults', 'STSPIN32G4_ClearFaults',
                 'STSPIN32G4_ReadStatus', 'HAL_GPIO_ReadPin', 'MotorControl_Stop',
                 'MotorControl_ResetTimerPhase'):
        address = f.symbols[name] & ~1
        f.uc.hook_add(UC_HOOK_CODE, hook, begin=address, end=address)
    f.uc.hook_add(UC_HOOK_CODE, hook, begin=callback, end=callback)
    def tick(uc, access, address, size, value, data):
        old = struct.unpack('<I', uc.mem_read(address, 4))[0]
        put(address, (old + 16000) & 0xFFFFFFFF)
    f.uc.hook_add(UC_HOOK_MEM_READ, tick, begin=0xE0001004, end=0xE0001007)
    def write(uc, access, address, size, value, data):
        if value & 0x8000:
            assert permitted, 'PWM enabled after permission expired'
            enabled.append(value)
    f.uc.hook_add(UC_HOOK_MEM_WRITE, write, begin=tim + 0x44, end=tim + 0x47)
    result = f.call('MotorControl_StartVoltageGuarded', callback | 1)
    assert result == (0 if cancel is None else 1), (cancel, result)
    assert len(enabled) == {None: 2, 'before_bootstrap': 0, 'after_bootstrap': 1}[cancel]
    if cancel:
        assert not struct.unpack('<I', f.uc.mem_read(tim + 0x44, 4))[0] & 0x8000


if __name__ == '__main__':
    for path in sys.argv[1:]:
        for cancel in (None, 'before_bootstrap', 'after_bootstrap'): run(path, cancel)
        print('PASS PWM bootstrap/final enable permission guards:', path)
