"""Run built ARM calibration code with simulated peripherals (never opens hardware).

python -m pip install --target build/cal_map_test_deps unicorn pyelftools
python tools/test_cal_map.py build/Release/HiradoraGen3FW.elf
PWM/ADC/encoder/VM and printf are stubbed; calibration, CRC, float math and
state transitions execute from the ELF. This does not verify ISR timing or UART DMA.
"""
import math
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
CONFIG = (ROOT / 'Core/Inc/motor_control_config.h').read_text(encoding='utf-8')
def configured_float(name):
    return float(re.search(r'^#define\s+'+name+r'\s+([0-9.]+)f',CONFIG,re.M)[1])
MAP_VOLTAGE = configured_float('MOTOR_CONTROL_CAL_MAP_VOLTAGE')
MAP_CURRENT_LIMIT = configured_float('MOTOR_CONTROL_CAL_MAP_CURRENT_LIMIT_A')
sys.path.insert(0, str(ROOT / "build/cal_map_test_deps"))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import *


class Firmware:
    SCRATCH = 0x20010000
    RETURN = 0x080F0000

    def __init__(self, path):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M4)
        self.uc.mem_map(0x08000000, 0x100000)
        self.uc.mem_map(0x20000000, 0x20000)
        self.uc.mem_map(0xE0000000, 0x100000)
        self.uc.mem_write(0xE000ED88, struct.pack('<I', 0xF00000))
        with open(path, 'rb') as stream:
            elf = ELFFile(stream)
            self.symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            for seg in elf.iter_segments():
                if seg['p_type'] == 'PT_LOAD' and seg['p_filesz']:
                    self.uc.mem_write(seg['p_vaddr'], seg.data())
        self.tick = 0
        self.seq = 0
        self.raw = 6.2
        self.direction = 1
        self.last_command = None
        self.voltage_mode = False
        self.adc = False
        self.valid = True
        self.vm = 24.0
        self.nfault = 1
        self.fresh = True
        self.travel_scale = 1.0
        self.log = []
        self.events = []
        hooks = {
            'HAL_GetTick': lambda: self.tick,
            'HAL_GPIO_ReadPin': lambda: self.nfault,
            'MotorControl_IsStopped': lambda: not self.voltage_mode,
            'MotorControl_IsVoltageMode': lambda: self.voltage_mode,
            'CurrentSense_IsBusy': lambda: self.adc,
            'MotorControl_StartVoltage': self.start,
            'MotorControl_Stop': self.stop,
            'MotorControl_SetVoltage': self.voltage,
            'CurrentSense_BeginControl': self.begin_adc,
            'CurrentSense_EndControl': self.end_adc,
            'AS5047P_GetSample': self.encoder,
            'BusVoltage_GetSample': self.bus,
            'CalibrationStore_Load': self.load,
            'CalibrationStore_IsCurrentFormat': lambda: True,
            'CalibrationStore_Save': self.forbidden,
            'printf': self.print_formatted,
            'puts': self.puts,
        }
        for name, fn in hooks.items():
            address = self.symbols[name] & ~1
            self.uc.hook_add(UC_HOOK_CODE, self.hook(fn), begin=address, end=address)

    def hook(self, fn):
        def callback(uc, address, size, data):
            result = fn()
            uc.reg_write(UC_ARM_REG_R0, int(result or 0) & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        return callback

    def call(self, name, *args):
        self.uc.reg_write(UC_ARM_REG_SP, 0x2001F000)
        self.uc.reg_write(UC_ARM_REG_LR, self.RETURN | 1)
        for reg, value in zip([UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3], args):
            self.uc.reg_write(reg, value)
        self.uc.emu_start(self.symbols[name] | 1, self.RETURN, count=10000000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == self.RETURN, name + ' did not return'
        return self.uc.reg_read(UC_ARM_REG_R0)

    def r0(self):
        return self.uc.reg_read(UC_ARM_REG_R0)

    def string(self, address):
        data = bytearray()
        while True:
            b = self.uc.mem_read(address + len(data), 1)[0]
            if not b:
                return data.decode('utf-8')
            data.append(b)

    def command(self, text):
        self.uc.mem_write(self.SCRATCH, text.encode() + b'\0')
        return self.call('MotorCalibration_ProcessCommand', self.SCRATCH)

    def puts(self):
        self.log.append(self.string(self.r0()) + '\n')

    def print_formatted(self):
        fmt = self.string(self.r0())
        regs = [self.uc.reg_read(r) for r in [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]]
        data = struct.pack('<4I', *regs) + bytes(self.uc.mem_read(self.uc.reg_read(UC_ARM_REG_SP), 512))
        offset = 4
        def replace(match):
            nonlocal offset
            spec, kind = match.group(0), match.group(1)
            if kind == '%':
                return '%'
            if kind == 'f':
                offset = (offset + 7) & ~7
                value = struct.unpack_from('<d', data, offset)[0]
                offset += 8
            else:
                value = struct.unpack_from('<i' if kind == 'd' else '<I', data, offset)[0]
                offset += 4
                if kind == 's':
                    value = self.string(value)
            return spec.replace('l', '') % value
        self.log.append(re.sub(r'%[-+0-9.]*l?([udscf%])', replace, fmt))
        return len(self.log[-1])

    def start(self):
        self.voltage_mode = True
        self.events.append('start')
        return 0

    def stop(self):
        self.voltage_mode = False
        self.events.append('stop')

    def begin_adc(self):
        self.adc = True
        return 0

    def end_adc(self):
        assert not self.voltage_mode, 'ADC ended before PWM stop'
        self.adc = False
        self.events.append('adc_end')

    def voltage(self):
        values = [struct.unpack('<f', struct.pack('<I', self.uc.reg_read(UC_ARM_REG_S0 + i)))[0] for i in range(4)]
        angle, vd, vq, vm = values
        assert 0 <= vd <= MAP_VOLTAGE+1e-5 and vq == 0 and vm >= 6
        if self.last_command is not None:
            delta = math.remainder(angle - self.last_command, 2 * math.pi)
            self.raw = (self.raw + self.direction * delta / 7 * self.travel_scale) % (2 * math.pi)
        self.last_command = angle
        return self.voltage_mode

    def encoder(self):
        if self.fresh:
            self.seq += 1
        self.uc.mem_write(self.r0(), struct.pack('<H2xffIIII?3x', 0, self.raw, 0, 0, 0, self.tick, self.seq, self.valid))
        return self.valid

    def bus(self):
        self.uc.mem_write(self.r0(), struct.pack('<ffIH??', self.vm, 3.3, self.tick, 0, True, self.vm > 30))
        return True

    def load(self):
        self.uc.mem_write(self.r0(), self.record)
        return True

    def forbidden(self):
        raise AssertionError('Flash write attempted')

    def initialize(self, direction=1):
        self.direction = direction
        offset = (direction * 7 * self.raw - 1.0) % (2 * math.pi)
        self.uc.reg_write(UC_ARM_REG_S0, struct.unpack('<I', struct.pack('<f', offset))[0])
        self.call('CalibrationStore_Make', self.SCRATCH + 256, direction & 0xFFFFFFFF)
        self.record = bytes(self.uc.mem_read(self.SCRATCH + 256, 40))
        self.call('MotorCalibration_Init')

    def unchanged(self):
        assert self.call('MotorCalibration_Get', self.SCRATCH + 256)
        assert bytes(self.uc.mem_read(self.SCRATCH + 256, 40)) == self.record
        self.command('cal status')
        assert 'stored=yes' in self.log[-1]

    def step(self, current=0.1, rails=False, dt=5):
        self.tick += dt
        if self.adc:
            self.uc.mem_write(self.SCRATCH + 128, struct.pack('<4f', current, 0.2, -0.3, 0.4))
            self.call('MotorCalibration_CurrentISR', self.SCRATCH + 128, int(rails))
            self.call('MotorCalibration_WatchdogISR')
        self.call('MotorCalibration_Task')

    def reach(self, stage):
        for _ in range(13000):
            self.command('cal status')
            if f'stage={stage},' in self.log[-1]:
                return
            self.step()
        raise AssertionError('Stage not reached: ' + stage)


def run_core(path):
    fw = Firmware(path)
    fw.command('cal test')
    assert '659 checks, 0 failures' in ''.join(fw.log), fw.log
    assert not fw.events
    print('cal test: 659 checks, 0 failures (ARM emulation)')
    fw.command('cal map')
    assert 'requires valid calibration' in fw.log[-1]
    for direction in [1, -1]:
        fw = Firmware(path)
        fw.initialize(direction)
        fw.command('cal map')
        for _ in range(13000):
            fw.step()
            if not fw.call('MotorCalibration_IsActive'):
                break
        output = ''.join(fw.log)
        assert 'CALMAP_END' in output, output[-1500:]
        rows = [line.split(',') for line in output.splitlines() if line.startswith('CALMAP,')]
        assert len(rows) == 2000
        for label in ['F', 'R']:
            selected = [r for r in rows if r[1] == label]
            assert [int(r[2]) for r in selected] == list(range(1000))
            assert all(abs(float(r[8])) < 0.02 for r in selected)
        assert not fw.voltage_mode and not fw.adc
        fw.unchanged()
        print(f'direction {direction:+}: 1000 + 1000 samples, wrap crossing, return, record/saved preserved')
    for stage in ['MAP_CHECK_STILL', 'MAP_RAMP', 'MAP_HOLD_START', 'MAP_FORWARD',
                  'MAP_HOLD_END', 'MAP_BACKWARD', 'MAP_HOLD_RETURN']:
        for stop in ['cal stop', 'stop_api']:
            fw = Firmware(path)
            fw.initialize()
            fw.command('cal map')
            fw.reach(stage)
            if stop == 'cal stop':
                fw.command(stop)
            else:
                fw.call('MotorCalibration_Stop')
            assert not fw.voltage_mode and not fw.adc
            assert not fw.call('MotorCalibration_IsActive')
            fw.unchanged()
    print('stop / cal stop: all 7 map stages preserve calibration and stop PWM before ADC')


def run_faults(path):
    for failure in ['overcurrent', 'rails', 'encoder', 'vm_low', 'vm_high', 'watchdog', 'adc_stale', 'nfault', 'slip', 'missed_sample']:
        fw = Firmware(path)
        fw.initialize()
        fw.command('cal map')
        fw.reach('MAP_FORWARD')
        if failure == 'overcurrent':
            fw.step(current=MAP_CURRENT_LIMIT+0.01)
        elif failure == 'rails':
            fw.step(rails=True)
        elif failure == 'encoder':
            fw.valid = False
        elif failure.startswith('vm_'):
            fw.vm = 5.9 if failure == 'vm_low' else 30.1
        elif failure in ['watchdog', 'adc_stale']:
            fw.tick += 21 if failure == 'watchdog' else 6
            fw.call('MotorCalibration_WatchdogISR')
        elif failure == 'nfault':
            fw.nfault = 0
        elif failure == 'slip':
            fw.travel_scale = 6 / 7
        elif failure == 'missed_sample':
            fw.fresh = False
            # 28ms間隔で少なくとも2つのtargetを跨ぎ、穴埋め禁止を検査する。
            for _ in range(16):
                fw.step()
            fw.fresh = True
        for _ in range(6000):
            fw.step()
            if not fw.call('MotorCalibration_IsActive'):
                break
        assert not fw.call('MotorCalibration_IsActive'), failure
        assert 'CALMAP_END' not in ''.join(fw.log), failure
        fw.unchanged()
    print('10 fault scenarios: stopped; no success summary; record/saved preserved')


if __name__ == '__main__':
    if '--faults-only' not in sys.argv:
        run_core(Path(sys.argv[1]))
    run_faults(Path(sys.argv[1]))
