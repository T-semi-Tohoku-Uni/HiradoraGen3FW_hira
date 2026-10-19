"""Exercise actual AS5600 ARM code with mocked I2C, no physical hardware.

Uses the existing Unicorn/pyelftools dependencies from test_current_pi.py.
"""
import math
import struct
import sys

from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                              UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_PC, UC_ARM_REG_LR)
from test_current_pi import Firmware


class Acquisition(Firmware):
    def stub(self, uc, address, size, data):
        # Execute the real stop routine when testing the common PWM start guard.
        if address == (self.symbols['MotorControl_Stop'] & ~1):
            return
        super().stub(uc, address, size, data)

    def __init__(self, path):
        super().__init__(path)
        self.local = {}
        with open(path, 'rb') as stream:
            source = ''
            for symbol in ELFFile(stream).get_section_by_name('.symtab').iter_symbols():
                if symbol['st_info']['type'] == 'STT_FILE':
                    source = symbol.name
                if source == 'as5600.c':
                    self.local[symbol.name] = symbol['st_value']
        self.uc.mem_map(0x40000000, 0x100000)
        self.now, self.launch_result, self.launches = 1, 0, []
        self.hi2c = self.symbols['hi2c1']
        self.put(self.hi2c, 0x40005400)
        self.put(self.local['encoder_i2c'], self.hi2c)
        self.put(self.local['state'], 1)
        self.put(self.symbols['SystemCoreClock'], 160000000)
        for name in ('HAL_GetTick', 'HAL_I2C_Mem_Read_DMA', 'HAL_I2C_GetError',
                     'HAL_DMA_Abort', 'HAL_I2C_DeInit', 'HAL_I2C_Init',
                     'HAL_I2CEx_ConfigAnalogFilter', 'HAL_I2CEx_ConfigDigitalFilter'):
            addr = self.symbols[name] & ~1
            self.uc.hook_add(UC_HOOK_CODE, self.mock, user_data=name, begin=addr, end=addr)

    def put(self, addr, value):
        self.uc.mem_write(addr, struct.pack('<I', value))

    def value(self, name, fmt='<I'):
        return struct.unpack(fmt, self.uc.mem_read(self.local[name], struct.calcsize(fmt)))[0]

    def mock(self, uc, address, size, name):
        result = 0
        if name == 'HAL_GetTick':
            result = self.now
        elif name == 'HAL_I2C_Mem_Read_DMA':
            assert uc.reg_read(UC_ARM_REG_R0) == self.hi2c
            assert uc.reg_read(UC_ARM_REG_R1) == 0x6c
            register = self.value('address', '<H')
            assert uc.reg_read(UC_ARM_REG_R2) == register
            assert uc.reg_read(UC_ARM_REG_R3) == 1  # 8-bit register address
            buffer, length = struct.unpack('<II', uc.mem_read(uc.reg_read(UC_ARM_REG_SP), 8))
            assert buffer == self.local['rx_bytes']
            assert length == (2 if register == 0x0c else 1)
            self.launches.append(register)
            result = self.launch_result
        elif name == 'HAL_I2C_GetError':
            result = 4  # HAL_I2C_ERROR_AF (NACK)
        uc.reg_write(UC_ARM_REG_R0, result)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def task(self, advance=1):
        self.now = (self.now + advance) & 0xffffffff
        self.call('AS5600_Task')

    def complete(self, data):
        self.uc.mem_write(self.local['rx_bytes'], bytes(data))
        self.call('HAL_I2C_MemRxCpltCallback', self.hi2c)

    def sample(self):
        valid = self.call('AS5600_GetSample', self.STATE)
        raw, angle, request, received, updated, seq, flag = struct.unpack(
            '<H2xfIIIIB', self.uc.mem_read(self.STATE, 25))
        return valid, raw, angle, seq


def test(path):
    f = Acquisition(path)
    assert not f.sample()[0]
    f.task()
    assert f.launches == [0x0b]  # Diagnose before trusting any angle.
    f.call('HAL_I2C_MemRxCpltCallback', f.hi2c + 4)
    assert f.value('state') == 2  # Other bus completion is ignored.
    f.complete([0x20])
    for raw in (0, 1024, 2048, 4095, 0, 0):
        f.task()
        assert f.launches[-1] == 0x0c
        count = len(f.launches)
        f.task()  # No overlapping DMA transaction.
        assert len(f.launches) == count
        old_seq = f.sample()[3]
        f.complete([raw >> 8, raw & 255])
        valid, got, angle, seq = f.sample()
        assert valid and got == raw and seq == old_seq + 1
        assert math.isclose(angle, raw * 2 * math.pi / 4096, abs_tol=1e-6)
    # Raw angle may wrap or stay unchanged; each completed read is a new sample.
    f.now += 50
    assert not f.sample()[0]
    f.task()
    assert f.launches[-1] == 0x0b
    f.complete([0x30])  # MD plus weak magnet must invalidate data.
    assert not f.sample()[0] and f.value('sensor_errors') == 1
    f.task()
    f.complete([0x20])
    f.task()
    f.complete([0x01, 0x23])
    assert f.sample()[0]
    f.task()
    old_seq = f.sample()[3]
    f.call('HAL_I2C_ErrorCallback', f.hi2c + 4)
    assert f.value('state') == 2
    f.call('HAL_I2C_ErrorCallback', f.hi2c)
    assert not f.sample()[0] and f.value('i2c_errors') == 1
    f.complete([0x0f, 0xff])  # Completion after failure cannot revive data.
    assert f.sample()[3] == old_seq
    f.task()
    assert f.value('state') == 4
    f.task(99)
    assert f.value('state') == 4
    f.task()
    assert f.launches[-1] == 0x0b  # Recheck magnet after reset.
    f.complete([0x20])
    f.task()
    # DWT unsigned wrap: timeout must still fire, without waiting for SysTick.
    f.put(f.local['start_cycles'], 0xffff0000)
    f.put(0xe0001004, (0xffff0000 + 800001) & 0xffffffff)
    f.task()
    assert f.value('timeouts') == 1 and not f.sample()[0]
    f.complete([0x03, 0x45])
    assert f.sample()[3] == old_seq
    f.task(100)
    f.complete([0x20])
    f.launch_result = 2  # HAL_BUSY at launch must not leave a REQUEST hanging.
    f.task()
    assert f.value('state') == 3 and f.value('i2c_errors') == 2
    f.task()
    f.task(100)
    f.call('AS5600_Pause')
    count = len(f.launches)
    f.task(100)
    assert f.value('state') == 0 and len(f.launches) == count
    assert not f.sample()[0]
    assert not f.call('AS5600_GetSample', 0)
    # Existing FOC-side SPI driver was not initialized by the AS5600 driver.
    assert not f.call('AS5047P_GetSample', f.STATE)
    f.put(f.symbols['motor_timer'], f.symbols['htim1'])
    f.put(f.symbols['htim1'], 0x40012c00)
    f.put(f.symbols['htim1'] + 20, 1)  # Init.RepetitionCounter
    f.put(0x40012c30, 1)  # RCR
    f.put(0x40012c00, 0x21)  # Center aligned 1 and CEN
    f.put(0x40012c44, 0x8000)  # MOE
    assert f.call('MotorControl_StartVoltage') == 1  # HAL_ERROR
    assert not struct.unpack('<I', f.uc.mem_read(0x40012c00, 4))[0] & 1
    assert not struct.unpack('<I', f.uc.mem_read(0x40012c44, 4))[0] & 0x8000
    # A delayed completion must be rejected even if main has not checked timeout.
    late = Acquisition(path)
    late.task()
    late.put(0xe0001004, 800001)
    late.complete([0x20])
    assert late.value('timeouts') == 1 and late.value('state') == 3
    assert not late.sample()[0]
    # Millisecond counter wrap must preserve stale detection.
    wrap = Acquisition(path)
    wrap.now = 0xfffffff0
    wrap.task()
    wrap.complete([0x20])
    wrap.task()
    wrap.complete([0, 1])
    assert wrap.sample()[0]
    wrap.now = (wrap.now + 50) & 0xffffffff
    assert not wrap.sample()[0]
    print(f'PASS: AS5600 acquisition, invalidation/recovery, wrap, PWM guard: {path}')


if __name__ == '__main__':
    for path in sys.argv[1:]:
        test(path)
