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
        self.put(self.local['sample_timer'], self.symbols['htim1'])
        self.put(self.symbols['htim1'], 0x40012c00)
        self.uc.mem_write(self.local['configured'], b'\1')
        self.uc.mem_write(self.local['state'], b'\1')
        self.put(self.symbols['SystemCoreClock'], 160000000)
        self.conf = 0xc0f5
        self.config_writes = []
        self.corrupt_readback = False
        self.reinitializations = 0
        for name in ('HAL_GetTick', 'HAL_I2C_Mem_Read_DMA', 'HAL_I2C_GetError',
                     'HAL_I2C_Mem_Read', 'HAL_I2C_Mem_Write', 'HAL_Delay',
                     'HAL_DMA_Abort', 'HAL_I2C_DeInit', 'I2C1_Reinitialize'):
            addr = self.symbols[name] & ~1
            self.uc.hook_add(UC_HOOK_CODE, self.mock, user_data=name, begin=addr, end=addr)

    def put(self, addr, value):
        self.uc.mem_write(addr, struct.pack('<I', value))

    def value(self, name, fmt='<I'):
        if name == 'state':
            fmt = '<B'  # Firmware toolchain uses short enums.
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
        elif name == 'I2C1_Reinitialize':
            self.reinitializations += 1
        elif name in ('HAL_I2C_Mem_Read', 'HAL_I2C_Mem_Write'):
            assert uc.reg_read(UC_ARM_REG_R1) == 0x6c
            assert uc.reg_read(UC_ARM_REG_R2) == 0x07  # Never write OTP!
            buffer, length = struct.unpack('<II', uc.mem_read(uc.reg_read(UC_ARM_REG_SP), 8))
            assert length == 2
            if name == 'HAL_I2C_Mem_Read':
                uc.mem_write(buffer, struct.pack('>H', self.conf ^
                    (0x0100 if self.corrupt_readback and self.config_writes else 0)))
            else:
                self.conf = struct.unpack('>H', uc.mem_read(buffer, 2))[0]
                self.config_writes.append(self.conf)
        uc.reg_write(UC_ARM_REG_R0, result)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def task(self, advance=1):
        self.now = (self.now + advance) & 0xffffffff
        cycles = struct.unpack('<I', self.uc.mem_read(0xe0001004, 4))[0]
        self.put(0xe0001004, (cycles + advance * 160000) & 0xffffffff)
        self.call('AS5600_Task')

    def complete(self, data):
        self.uc.mem_write(self.local['rx_bytes'], bytes(data))
        self.call('HAL_I2C_MemRxCpltCallback', self.hi2c)

    def sample(self):
        valid = self.call('AS5600_GetSample', self.STATE)
        raw, angle, electrical, request, received, updated, seq, flag, reference = struct.unpack(
            '<H2xffIIIIB3xI', self.uc.mem_read(self.STATE, 36))
        assert reference == request
        assert math.isclose(electrical, ((raw*7) % 4096)*2*math.pi/4096, abs_tol=1e-6)
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
        f.task(0)  # No overlapping DMA transaction, without exceeding timeout.
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
    assert f.config_writes == [0xc3f0]  # Preserve output/reserved bits.
    f.complete([0x20])
    f.task()
    # DWT unsigned wrap: timeout must still fire, without waiting for SysTick.
    f.put(f.local['start_cycles'], 0xffff0000)
    f.put(0xe0001004, (0xffff0000 + 800001) & 0xffffffff)
    f.task(0)
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
    assert 'AS5047P_GetSample' not in f.symbols  # Legacy SPI driver no longer linked.
    # The bring-up build blocks PWM. Energized builds are tested without starting peripherals.
    import re
    from pathlib import Path
    config = (Path(__file__).resolve().parents[1] / 'Core/Inc/motor_control_config.h').read_text(encoding='utf-8')
    if re.search(r'#define\s+MOTOR_CONTROL_ANGLE_ACQUISITION_ONLY\s+1\b', config):
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
    # Timer path: 200 us rate limit, main does not launch while TIM1 runs.
    fast = Acquisition(path)
    fast.put(0x40012c00, 1)
    fast.call('AS5600_Tick')
    fast.put(0xe0001004, 16000)
    fast.complete([0x20])
    fast.put(0xe0001004, 31999)
    fast.call('AS5600_Tick')
    assert len(fast.launches) == 1
    fast.put(0xe0001004, 32000)
    fast.call('AS5600_Tick')
    fast.put(0xe0001004, 48000)
    fast.complete([0x0f, 0xfa])
    fast.task(0)
    assert len(fast.launches) == 2
    fast.put(0xe0001004, 64000)
    fast.call('AS5600_Tick')
    fast.put(0xe0001004, 80000)
    fast.complete([0, 5])
    assert fast.sample()[1] == 5
    # A periodic diagnostic consumes one slot, not an unbounded angle pause.
    fast.now = 21
    fast.put(0xe0001004, 96000)
    fast.call('AS5600_Tick')
    assert fast.launches[-1] == 0x0b
    fast.put(0xe0001004, 112000)
    fast.complete([0x20])
    fast.put(0xe0001004, 128000)
    fast.call('AS5600_Tick')
    fast.put(0xe0001004, 144000)
    fast.complete([0, 6])
    assert fast.sample()[0]
    assert fast.value('tim_interval_max_cycles') == 64000  # Diagnostic -> 400 us gap.
    assert struct.unpack('<I', fast.uc.mem_read(fast.STATE+32, 4))[0] == 128000
    fast.put(0xe0001004, 160000)
    fast.call('AS5600_Tick')
    assert fast.value('tim_age_max_cycles') == 64000
    fast.put(0xe0001004, 208001)  # 300 us timeout checked by Tick, without main.
    fast.call('AS5600_Tick')
    assert fast.value('state') == 3 and not fast.sample()[0]
    fast.uc.mem_write(fast.symbols['outputs_enabled'], b'\1')
    fast.task(0)
    assert fast.value('state') == 3  # Do not reconfigure the bus during drive.
    fast.uc.mem_write(fast.symbols['outputs_enabled'], b'\0')
    fast.task(0)
    assert fast.value('state') == 4
    fast.uc.mem_write(fast.symbols['outputs_enabled'], b'\1')
    fast.task(100)
    assert fast.value('state') == 4  # Recheck stopped state after retry delay too.
    fast.uc.mem_write(fast.symbols['outputs_enabled'], b'\0')
    fast.task(0)
    assert fast.value('state') == 1
    assert fast.reinitializations == 1  # Recovery delegates to CubeMX initialization.
    rate_wrap = Acquisition(path)
    rate_wrap.uc.mem_write(rate_wrap.local['launched'], b'\1')
    rate_wrap.put(rate_wrap.local['last_start_cycles'], 0xfffff000)
    rate_wrap.put(0xe0001004, (0xfffff000+31999) & 0xffffffff)
    rate_wrap.call('AS5600_Tick')
    assert not rate_wrap.launches
    rate_wrap.put(0xe0001004, (0xfffff000+32000) & 0xffffffff)
    rate_wrap.call('AS5600_Tick')
    assert len(rate_wrap.launches) == 1
    mismatch = Acquisition(path)
    mismatch.call('AS5600_Pause')
    mismatch.corrupt_readback = True
    mismatch.call('AS5600_Resume')
    assert mismatch.value('state') == 3 and not mismatch.sample()[0]
    print(f'PASS: AS5600 acquisition, invalidation/recovery, wrap, PWM guard when configured: {path}')


if __name__ == '__main__':
    for path in sys.argv[1:]:
        test(path)
