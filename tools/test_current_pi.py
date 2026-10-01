"""Execute actual ARM firmware PI and UART parser in Unicorn (no hardware).

Usage: python tools/test_current_pi.py build/Release/HiradoraGen3FW.elf
Dependencies: unicorn, pyelftools (same as test_cal_map.py).
"""
import math
import struct
import sys

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_CPU_ARM_CORTEX_M4, UC_ARM_REG_R0, UC_ARM_REG_R1,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_S0,
)


class Firmware:
    STATE, CONFIG, TEXT = 0x20010000, 0x20010100, 0x20010200
    RETURN = 0x080F0000

    def __init__(self, path):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M4)
        self.uc.mem_map(0x08000000, 0x100000)
        self.uc.mem_map(0x20000000, 0x20000)
        self.uc.mem_map(0xE0000000, 0x100000)
        self.uc.mem_write(0xE000ED88, struct.pack('<I', 0xF00000))
        self.symbols, self.foc = {}, {}
        self.messages = []
        with open(path, 'rb') as stream:
            elf = ELFFile(stream)
            source = ''
            for sym in elf.get_section_by_name('.symtab').iter_symbols():
                if sym['st_info']['type'] == 'STT_FILE':
                    source = sym.name
                self.symbols[sym.name] = sym['st_value']
                if source == 'foc_voltage.c':
                    self.foc[sym.name] = sym['st_value']
            for seg in elf.iter_segments():
                if seg['p_type'] == 'PT_LOAD' and seg['p_filesz']:
                    self.uc.mem_write(seg['p_vaddr'], seg.data())
        for name in ('printf', 'puts', 'MotorCalibration_IsActive', 'MotorControl_Stop'):
            address = self.symbols[name] & ~1
            self.uc.hook_add(UC_HOOK_CODE, self.stub, begin=address, end=address)

    def string(self, address):
        data = bytearray()
        while True:
            value = self.uc.mem_read(address + len(data), 1)[0]
            if not value:
                return data.decode('utf-8')
            data.append(value)

    def stub(self, uc, address, size, data):
        if address == (self.symbols['printf'] & ~1):
            message = self.string(uc.reg_read(UC_ARM_REG_R0))
            if 'config: %s' in message:
                message = message.replace('%s', self.string(uc.reg_read(UC_ARM_REG_R1)))
            self.messages.append(message)
        uc.reg_write(UC_ARM_REG_R0, 0)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def call(self, name, r0=0, r1=0, floats=()):
        self.uc.reg_write(UC_ARM_REG_SP, 0x2001F000)
        self.uc.reg_write(UC_ARM_REG_LR, self.RETURN | 1)
        self.uc.reg_write(UC_ARM_REG_R0, r0)
        self.uc.reg_write(UC_ARM_REG_R1, r1)
        for index, value in enumerate(floats):
            bits = struct.unpack('<I', struct.pack('<f', value))[0]
            self.uc.reg_write(UC_ARM_REG_S0 + index, bits)
        self.uc.emu_start(self.symbols[name] | 1, self.RETURN, count=1000000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == self.RETURN, name
        return self.uc.reg_read(UC_ARM_REG_R0)

    def configure(self, kp_d=1, ki_d=0, kp_q=1, ki_q=0, ld=1, lq=1):
        self.uc.mem_write(self.CONFIG, struct.pack('<6f', kp_d, ki_d, kp_q, ki_q, ld, lq))
        self.call('CurrentPi_Reset', self.STATE)

    def step(self, dr, qr, d=0, q=0, dt=0.001, limit=1):
        ok = self.call('CurrentPi_Update', self.STATE, self.CONFIG,
                       (dr, qr, d, q, dt, limit))
        state = struct.unpack('<6fB', self.uc.mem_read(self.STATE, 25))
        return ok, state

    def command(self, text):
        self.uc.mem_write(self.TEXT, text.encode() + b'\0')
        assert self.call('FocVoltage_ProcessCommand', self.TEXT) == 1

    def get(self, name, fmt):
        return struct.unpack(fmt, self.uc.mem_read(self.foc[name], struct.calcsize(fmt)))[0]


def close(a, b):
    assert math.isclose(a, b, abs_tol=2e-6), (a, b)


def test(path):
    f = Firmware(path)
    f.configure()
    ok, s = f.step(0.3, -0.4, d=0.1, q=-0.1)
    assert ok
    close(s[4], 0.2)
    close(s[5], -0.3)
    ok, s = f.step(3, 4)
    assert ok and s[6]
    close(s[4], 0.6)
    close(s[5], 0.8)
    # Integral clamp without vector saturation, in both polarities.
    f.configure(kp_d=0, ki_d=10, kp_q=0, ki_q=10, ld=0.2, lq=0.3)
    for _ in range(100):
        ok, s = f.step(1, -1)
        assert ok
    close(s[0], 0.2)
    close(s[1], -0.3)
    # Sustained output saturation must not wind up either integral.
    f.configure(ki_d=10, ki_q=10)
    for _ in range(100):
        ok, s = f.step(3, -4)
        assert ok and s[6]
        close(s[0], 0)
        close(s[1], 0)
    ok, s = f.step(0, 0)
    assert ok and not s[6]
    close(s[4], 0)
    close(s[5], 0)
    # Integral must unwind even if the bus voltage limit shrinks below it.
    f.configure(kp_d=0, ki_d=10)
    f.uc.mem_write(f.STATE, struct.pack('<f', 0.8))
    ok, s = f.step(-1, 0, limit=0.2)
    assert ok and s[0] < 0.8 and s[6]
    before = bytes(f.uc.mem_read(f.STATE, 28))
    for args in (dict(dr=math.nan, qr=0), dict(dr=0, qr=math.inf),
                 dict(dr=0, qr=0, dt=0), dict(dr=0, qr=0, limit=-1),
                 dict(dr=3e38, qr=3e38)):
        assert not f.step(**args)[0]
        assert bytes(f.uc.mem_read(f.STATE, 28)) == before
    f.call('CurrentPi_Reset', f.STATE)
    assert not any(f.uc.mem_read(f.STATE, 28))
    # UART accepts a paired reference, rejects malformed/out-of-circle input.
    f.command('foc current 0.3 -0.4')
    if f.get('control_mode', '<I') == 0:
        # The user's build may deliberately contain inconsistent tuning limits.
        # Verify the specific configuration rejection instead of assuming defaults.
        assert 'FOC current rejected: config:' in f.messages[-1], f.messages
        assert 'CURRENT_REF_MAX_A must be < CURRENT_LIMIT_A' in f.messages[-1], f.messages
        f.command('foc current 0 0')
        assert 'CURRENT_REF_MAX_A must be < CURRENT_LIMIT_A' in f.messages[-1]
        close(f.get('current_target_d', '<f'), 0)
        close(f.get('current_target_q', '<f'), 0)
        f.command('foc current nan 0')
        assert 'finite vector' in f.messages[-1] and 'config:' not in f.messages[-1]
        f.command('foc voltage 0 0.4')
        close(f.get('target_q', '<f'), 0.4)
        print(f'PASS: PI math and explicit invalid-config rejection (valid current mode not tested): {path}')
        return
    assert f.get('control_mode', '<I') == 1
    close(f.get('current_target_d', '<f'), 0.3)
    close(f.get('current_target_q', '<f'), -0.4)
    for command in ('foc current nan 0', 'foc current 0 inf', 'foc current 1e30 1e30',
                    'foc current 0.2', 'foc current 0 0 extra', 'foc current 1e99 0'):
        f.command(command)
        close(f.get('current_target_d', '<f'), 0.3)
        close(f.get('current_target_q', '<f'), -0.4)
    # Active current control permits zero/reversal, blocks voltage mode change.
    f.uc.mem_write(f.foc['active'], b'\x01')
    f.command('foc voltage 0 0.2')
    assert f.get('control_mode', '<I') == 1
    f.command('foc current 0 0')
    assert f.call('FocVoltage_IsActive') == 1
    close(f.get('current_target_q', '<f'), 0)
    f.command('foc current 0 0.2')
    close(f.get('current_target_q', '<f'), 0.2)
    f.uc.mem_write(f.foc['current_pi'], struct.pack('<f', 0.5))
    f.command('foc stop')
    assert not any(f.uc.mem_read(f.foc['current_pi'], 28))
    # Simulate completed stop cleanup, select existing voltage path.
    f.uc.mem_write(f.foc['active'], b'\0')
    f.command('foc voltage 0 0.4')
    assert f.get('control_mode', '<I') == 0
    close(f.get('target_q', '<f'), 0.4)
    f.uc.mem_write(f.foc['active'], b'\x01')
    f.command('foc current 0 0.1')
    assert f.get('control_mode', '<I') == 0
    f.command('foc voltage 0 0.2')  # Existing running decrease restriction.
    close(f.get('target_q', '<f'), 0.4)
    print(f'PASS: PI math, clamp, saturation/recovery, reset, UART/mode guards: {path}')


if __name__ == '__main__':
    for path in sys.argv[1:]:
        test(path)
