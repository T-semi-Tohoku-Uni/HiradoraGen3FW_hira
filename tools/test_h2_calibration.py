"""ARM ELF tests for streaming fitting, record migration/CRC and map apply.
Uses the same Unicorn/pyelftools dependencies as test_cal_map.py.
python tools/test_h2_calibration.py build/Debug/HiradoraGen3FW.elf
"""
import math
import struct
import sys
import zlib
from test_cal_map import Firmware as MapFirmware
from test_current_pi import Firmware
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import *


def fit_tests(path):
    f = Firmware(path)
    for c, a, b in ((0.4, -0.051, 0.055), (-0.8, 0.2, -0.1), (0, 0, 0)):
        f.uc.mem_write(f.STATE, bytes(80))
        for i in range(1000):
            theta = 2*math.pi*(i/999)**1.4  # Deliberately nonuniform, including endpoints.
            error = c+a*math.cos(2*theta)+b*math.sin(2*theta)
            f.call('H2Fit_Add', f.STATE, floats=(theta, error))
        assert f.call('H2Fit_Solve', f.STATE, f.CONFIG)
        actual = struct.unpack('<3f', f.uc.mem_read(f.CONFIG, 12))
        assert all(abs(x-y)<2e-6 for x,y in zip(actual,(c,a,b))), actual
    for values in ([(0,0)]*10, [(0,0),(1,0)], [(0,3.13),(0.1,-3.13)],
                   [(0,0),(1,float('nan'))], [(float('inf'),0)]):
        f.uc.mem_write(f.STATE, bytes(80))
        for theta,error in values:
            f.call('H2Fit_Add',f.STATE,floats=(theta,error))
        f.uc.mem_write(f.CONFIG,b'X'*12)
        assert not f.call('H2Fit_Solve',f.STATE,f.CONFIG)
        assert bytes(f.uc.mem_read(f.CONFIG,12))==b'X'*12
    print('PASS: nonuniform least squares, DC separation, zero, singular, NaN/Inf and wrap rejection')


def storage_tests(path):
    f = Firmware(path)
    flash = f.symbols['__calibration_start__']
    f.call('CalibrationStore_Make',f.STATE,1,floats=(0.73,))
    f.call('CalibrationStore_SetH2',f.STATE,floats=(-0.051,0.055))
    record=bytes(f.uc.mem_read(f.STATE,40))
    assert struct.unpack_from('<I',record)[0]==2
    assert zlib.crc32(record[:36])==struct.unpack_from('<I',record,36)[0]
    for bit in range(320):
        bad=bytearray(record); bad[bit//8]^=1<<(bit%8)
        f.uc.mem_write(f.STATE,bytes(bad))
        assert not f.call('CalibrationStore_Valid',f.STATE)
    f.uc.mem_write(f.STATE,record)
    # Exercise real Save logic, stubbing only peripherals and idle guards.
    f.uc.mem_map(0x40000000,0x100000)
    written=[]
    def stub(uc,address,size,data):
        ret=0
        if address==(f.symbols['MotorControl_IsStopped'] & ~1): ret=1
        elif address==(f.symbols['HAL_FLASHEx_Erase'] & ~1):
            uc.mem_write(flash,b'\xff'*2048)
        elif address==(f.symbols['HAL_FLASH_Program'] & ~1):
            target=uc.reg_read(UC_ARM_REG_R1)
            uc.mem_write(target,struct.pack('<II',uc.reg_read(UC_ARM_REG_R2),uc.reg_read(UC_ARM_REG_R3)))
            written.append(target)
        uc.reg_write(UC_ARM_REG_R0,ret)
        uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    for name in ('MotorControl_IsStopped','CurrentSense_IsBusy','HAL_FLASH_Unlock',
                 'HAL_FLASH_Lock','HAL_FLASHEx_Erase','HAL_FLASH_Program'):
        addr=f.symbols[name] & ~1
        f.uc.hook_add(UC_HOOK_CODE,stub,begin=addr,end=addr)
    assert f.call('CalibrationStore_Save',f.STATE)
    assert written==list(range(flash,flash+40,8))
    assert f.call('CalibrationStore_Load',f.CONFIG)
    assert bytes(f.uc.mem_read(f.CONFIG,40))==record
    assert f.call('CalibrationStore_IsCurrentFormat')
    for value in (float('nan'),float('inf')):
        f.uc.mem_write(f.STATE,record)
        f.call('CalibrationStore_SetH2',f.STATE,floats=(value,0.01))
        assert not f.call('CalibrationStore_Valid',f.STATE)
    for length in (0,8,16,24,32):
        f.uc.mem_write(flash,record[:length]+b'\xff'*(40-length))
        assert not f.call('CalibrationStore_Load',f.CONFIG)
    legacy=bytearray(record[:24])
    struct.pack_into('<I',legacy,0,1)
    legacy+=struct.pack('<I',0x43414c31)
    legacy+=struct.pack('<I',zlib.crc32(legacy))
    f.uc.mem_write(flash,bytes(legacy)+b'\xff'*8)
    assert f.call('CalibrationStore_Load',f.CONFIG)
    migrated=bytes(f.uc.mem_read(f.CONFIG,40))
    assert migrated[4:24]==record[4:24]
    assert migrated[24:32]==bytes(8)
    assert f.call('CalibrationStore_Valid',f.CONFIG)
    assert not f.call('CalibrationStore_IsCurrentFormat')
    assert bytes(f.uc.mem_read(flash,32))==legacy  # No auto-save during migration.
    for kind in ('crc','identity','nonfinite','unknown'):
        bad=bytearray(legacy)
        if kind=='crc': bad[-1]^=1
        if kind=='identity':
            struct.pack_into('<I',bad,4,0xffffffff)
        if kind=='nonfinite': struct.pack_into('<f',bad,20,float('nan'))
        if kind=='unknown': struct.pack_into('<I',bad,0,99)
        if kind!='crc': struct.pack_into('<I',bad,28,zlib.crc32(bad[:28]))
        f.uc.mem_write(flash,bytes(bad)+b'\xff'*8)
        f.uc.mem_write(f.CONFIG,b'X'*40)
        assert not f.call('CalibrationStore_Load',f.CONFIG)
        assert bytes(f.uc.mem_read(f.CONFIG,40))==b'X'*40
    print('PASS: v2 save/readback, commit order, all CRC bits, v1 migration and invalid record preservation')


class SyntheticMap(MapFirmware):
    def __init__(self,path):
        super().__init__(path)
        self.reverse=False
        self.saved_record=None
        for name in ('Console_Flush','AS5047P_Pause','AS5047P_Resume'):
            address=self.symbols[name] & ~1
            self.uc.hook_add(UC_HOOK_CODE,self.hook(lambda:0),begin=address,end=address)

    def forbidden(self):
        self.saved_record=bytes(self.uc.mem_read(self.r0(),40))
        return True

    def voltage(self):
        angle=struct.unpack('<f',struct.pack('<I',self.uc.reg_read(UC_ARM_REG_S0)))[0]
        if self.last_command is not None:
            delta=math.remainder(angle-self.last_command,2*math.pi)
            if abs(delta)>1e-6: self.reverse=delta<0
        return super().voltage()

    def encoder(self):
        # Solve measured = ideal + error(measured)/(direction*p).
        # Distinct F/R harmonics and DC offsets test independent fits and averaging.
        theta=self.raw
        c,a,b=(-0.01,-0.04,0.06) if self.reverse else (0.01,-0.06,0.04)
        for _ in range(12):
            theta=self.raw+(c+a*math.cos(2*theta)+b*math.sin(2*theta))/(self.direction*7)
        raw=self.raw
        self.raw=theta%(2*math.pi)
        result=super().encoder()
        self.raw=raw
        return result


def map_tests(path):
    for direction in (-1,1):
        f=SyntheticMap(path); f.initialize(direction)
        f.command('cal map apply'); f.unchanged()
        f.command('cal map')
        for _ in range(13000):
            f.step()
            if not f.call('MotorCalibration_IsActive'): break
        assert 'CALMAP_END' in ''.join(f.log), ''.join(f.log[-10:])
        f.unchanged()
        # save before apply must persist the current record, not the candidate.
        f.command('cal save'); assert f.saved_record==f.record
        f.adc=True; f.command('cal map apply'); f.adc=False; f.unchanged()
        f.voltage_mode=True; f.command('cal map apply'); f.voltage_mode=False; f.unchanged()
        active=Firmware(path).foc['active']
        f.uc.mem_write(active,b'\x01'); f.command('cal map apply')
        f.uc.mem_write(active,b'\0'); f.unchanged()
        # Changed calibration must not accept a candidate from a different record.
        addr=f.symbols['record']
        f.uc.mem_write(addr+20,struct.pack('<f',0.123))
        f.command('cal map apply')
        assert 'current calibration' in f.log[-1]
        f.uc.mem_write(addr,f.record)
        f.command('cal map apply')
        assert f.call('MotorCalibration_Get',f.SCRATCH+256)
        applied=bytes(f.uc.mem_read(f.SCRATCH+256,40))
        assert applied[:24]==f.record[:24]
        a,b=struct.unpack_from('<2f',applied,24)
        assert abs(a+0.05)<2e-5 and abs(b-0.05)<2e-5,(a,b)
        f.command('cal status'); assert 'stored=no' in f.log[-1]
        f.command('cal save'); assert f.saved_record==applied
        f.record=applied
        f.unchanged()
        # A failed subsequent map cannot apply an older candidate or destroy current values.
        f.command('cal map'); f.command('cal stop'); f.command('cal map apply'); f.unchanged()
        f.call('MotorCalibration_Init'); f.unchanged()
    print('PASS: full F/R fit, average, apply guards, unchanged direction/offset, save separation, abort and reboot')


if __name__=='__main__':
    for path in sys.argv[1:]:
        fit_tests(path)
        storage_tests(path)
        map_tests(path)
        print('PASS:',path)
