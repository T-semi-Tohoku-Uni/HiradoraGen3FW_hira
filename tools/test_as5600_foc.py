"""AS5600 DMA publication -> actual FOC/Park, protection and CAN angle units.

Only hardware I2C, CORDIC sin/cos and peripheral FIFO state are modeled.
No physical hardware is accessed, and PWM starts remain disabled in firmware.
"""
import math
import struct
import sys
import zlib

from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_S0, UC_ARM_REG_PC, UC_ARM_REG_LR
from test_as5600 import Acquisition


def test(path):
    f = Acquisition(path)
    f.task()
    f.complete([0x20])
    f.task()
    f.complete([0x03, 0xe8])  # 1000 / 4096 turn, published by actual driver.
    reference = struct.unpack('<I', f.uc.mem_read(f.local['samples']+36+32, 4))[0]
    assert reference == 320000
    for name, value in (('active', 1), ('running', 1)):
        f.uc.mem_write(f.foc[name], bytes([value]))
    f.uc.mem_write(f.foc['calibration']+16, struct.pack('<ifff', 1, .23, 0., 0.))
    f.uc.mem_write(f.foc['control_period'], struct.pack('<f', .00005))
    f.put(f.symbols['motor_timer'], f.symbols['htim1'])
    f.put(f.symbols['control_tick_hz'], 20000)
    f.put(0x40012c2c, 4000)
    f.put(0x40012c40, 3999)
    f.put(f.symbols['foc_adc_began'], reference+24300)
    f.put(f.symbols['foc_adc_phase_cycles'], reference+24300)
    f.put(f.symbols['foc_adc_phase'], 300)
    f.put(0xe0001004, reference+24400)

    def trig(uc, address, size, data):
        angle = struct.unpack('<f', struct.pack('<I', uc.reg_read(UC_ARM_REG_S0)))[0]
        uc.mem_write(uc.reg_read(UC_ARM_REG_R0), struct.pack('<f', math.sin(angle)))
        uc.mem_write(uc.reg_read(UC_ARM_REG_R1), struct.pack('<f', math.cos(angle)))
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    addr = f.symbols['VoltageVector_SinCosWrapped'] & ~1
    f.uc.hook_add(UC_HOOK_CODE, trig, begin=addr, end=addr)
    f.uc.mem_write(f.STATE, struct.pack('<4f', 1., -.5, 1., -.5))
    f.call('FocVoltage_CurrentISR', f.STATE, 0)
    assert f.get('observation_valid', '<?')
    expected = (7*1000*2*math.pi/4096-.23) % (2*math.pi)
    assert abs(f.get('observed_base_electrical', '<f')-expected) < 2e-6
    assert abs(f.get('observed_electrical', '<f')-expected) < 2e-6
    assert struct.unpack('<I', f.uc.mem_read(f.foc['cycle_angle']+32, 4))[0] == reference
    assert f.get('prediction_pwm_seconds', '<f') > f.get('prediction_adc_seconds', '<f')
    before = bytes(f.uc.mem_read(f.foc['angle_predictor'], 8))
    f.call('FocVoltage_CurrentISR', f.STATE, 0)
    assert bytes(f.uc.mem_read(f.foc['angle_predictor'], 8)) == before
    # The actual current ISR must trip above the requested 10 A phase limit.
    f.uc.mem_write(f.STATE, struct.pack('<4f', 10., 0., 0., 0.))
    f.call('FocVoltage_CurrentISR', f.STATE, 0)
    assert f.get('running', '<?')
    f.uc.mem_write(f.STATE, struct.pack('<4f', 10.01, 0., 0., 0.))
    f.call('FocVoltage_CurrentISR', f.STATE, 0)
    assert not f.get('running', '<?')
    assert f.string(f.get('fault', '<I')) == 'phase overcurrent'
    f.put(f.foc['fault'], 0)
    f.uc.mem_write(f.foc['running'], b'\1')
    f.uc.mem_write(f.STATE, bytes(16))
    # Data remains valid to GetSample, but is too old for FOC after 750 us.
    f.put(0xe0001004, reference+120001)
    f.call('FocVoltage_CurrentISR', f.STATE, 0)
    assert not f.get('running', '<?')
    assert f.string(f.get('fault', '<I')) == 'encoder age limit'
    # A failed I2C transfer invalidates the next FOC snapshot immediately.
    f.put(f.foc['fault'], 0)
    f.uc.mem_write(f.foc['running'], b'\1')
    f.call('AS5600_Tick')
    f.call('HAL_I2C_ErrorCallback', f.hi2c)
    f.call('FocVoltage_CurrentISR', f.STATE, 0)
    assert not f.get('running', '<?')
    assert f.string(f.get('fault', '<I')) == 'encoder invalid'
    f.call('CalibrationStore_Make', f.CONFIG, 1, floats=(.23,))
    record = bytearray(f.uc.mem_read(f.CONFIG, 40))
    assert struct.unpack_from('<IIIf', record) == (2, 2, 7, 360.)
    assert f.call('CalibrationStore_Valid', f.CONFIG)
    struct.pack_into('<I', record, 4, 1)  # Old motor identity, with a valid CRC.
    struct.pack_into('<f', record, 12, 140.)
    struct.pack_into('<I', record, 36, zlib.crc32(record[:36]))
    f.uc.mem_write(f.CONFIG, bytes(record))
    assert not f.call('CalibrationStore_Valid', f.CONFIG)

    # Exercise actual CanComm_Task using a healthy actual sensor, not a raw stub.
    c = Acquisition(path)
    c.task(); c.complete([0x20]); c.task(); c.complete([0x0f, 0xff])
    can = {}
    with open(path, 'rb') as stream:
        source = ''
        for symbol in ELFFile(stream).get_section_by_name('.symtab').iter_symbols():
            if symbol['st_info']['type'] == 'STT_FILE': source = symbol.name
            if source == 'can_comm.c': can[symbol.name] = symbol['st_value']
    c.uc.mem_write(can['ready'], b'\1')
    c.put(can['tx_ms'], c.now)  # No wire transmission in this test.
    def fifo(uc, address, size, data):
        uc.reg_write(UC_ARM_REG_R0, 0)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    addr = c.symbols['HAL_FDCAN_GetRxFifoFillLevel'] & ~1
    c.uc.hook_add(UC_HOOK_CODE, fifo, begin=addr, end=addr)
    c.call('CanComm_Task')
    telemetry = bytes(c.uc.mem_read(can['telemetry'], 20))
    assert struct.unpack_from('<H', telemetry, 14)[0] == 8190  # raw 4095 -> 13-bit wire angle.
    print('PASS: real AS5600->FOC snapshot/Park, 10 A trip, 750 us age, I2C invalidation, CAN units:', path)


if __name__ == '__main__':
    for path in sys.argv[1:]:
        test(path)
