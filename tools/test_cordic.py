"""Execute ARM conversion/MMIO code with an ideal CORDIC peripheral model.

This checks the software protocol, not hardware accuracy or execution time.
Run cal trig test on hardware for those measurements.
Usage: python tools/test_cordic.py cmsis.elf cordic.elf [...]
"""
import math
import struct
import sys
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_PRIMASK
from test_current_pi import Firmware
from test_foc_trig import pair, f32


class CordicFirmware(Firmware):
    BASE = 0x40020C00

    def __init__(self, path):
        super().__init__(path)
        self.uc.mem_map(0x40000000, 0x40000)
        self.arguments, self.results = [], []
        self.transactions = 0
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write, begin=self.BASE+4, end=self.BASE+4)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.read, begin=self.BASE+8, end=self.BASE+8)
        self.call('VoltageVector_Init')
        csr = struct.unpack('<I', self.uc.mem_read(self.BASE, 4))[0]
        assert csr == 0x00180060, hex(csr)  # cosine, 6 cycles, 2 writes/reads, Q31

    def write(self, uc, access, address, size, value, data):
        assert uc.reg_read(UC_ARM_REG_PRIMASK) == 1
        assert not self.results, 'previous result not drained'
        self.arguments.append(value)
        if len(self.arguments) == 2:
            angle, modulus = self.arguments
            assert modulus == 0x7fffffff
            angle = struct.unpack('<i', struct.pack('<I', angle))[0] * math.pi / 2**31
            def q31(x):
                return max(-2**31, min(2**31-1, round(x*(2**31-1))))
            self.results = [q31(math.cos(angle)), q31(math.sin(angle))]
            self.arguments.clear()
            self.transactions += 1

    def read(self, uc, access, address, size, value, data):
        assert uc.reg_read(UC_ARM_REG_PRIMASK) == 1
        assert self.results, 'read before two arguments'
        uc.mem_write(address, struct.pack('<i', self.results.pop(0)))


def test(reference, candidate):
    old, new = Firmware(reference), CordicFirmware(candidate)
    two_pi = f32(2*math.pi)
    angles = [f32(i*two_pi/4096) for i in range(4097)]
    angles += [0., -0., f32(-1e-8), two_pi, f32(two_pi-5e-7),
               f32(two_pi/2), f32(two_pi/2-3e-7), f32(two_pi/2+3e-7)]
    pair_error = duty_error = 0.
    for angle in angles:
        s, c = pair(new, 'VoltageVector_SinCos', angle)
        rs, rc = pair(old, 'VoltageVector_SinCos', angle)
        pair_error = max(pair_error, abs(s-rs), abs(c-rc))
        assert max(abs(s-rs), abs(c-rc)) < 5e-6, (angle,s,c,rs,rc)
        for vd,vq,vm in ((.3,-.2,24), (0,0,24), (100,-100,6)):
            assert new.call('VoltageVector_ComputeSinCos',new.TEXT,floats=(s,c,vd,vq,vm,3,.05))
            assert old.call('VoltageVector_ComputeSinCos',old.TEXT,floats=(rs,rc,vd,vq,vm,3,.05))
            d = struct.unpack('<3f',new.uc.mem_read(new.TEXT,12))
            r = struct.unpack('<3f',old.uc.mem_read(old.TEXT,12))
            duty_error=max(duty_error,*(abs(a-b) for a,b in zip(d,r)))
            assert max(abs(a-b) for a,b in zip(d,r)) < 3e-6
    for state in (0,1):
        new.uc.reg_write(UC_ARM_REG_PRIMASK,state)
        pair(new,'VoltageVector_SinCosWrapped',1.)
        assert new.uc.reg_read(UC_ARM_REG_PRIMASK)==state
    for angle in (math.nan,math.inf,-math.inf):
        count=new.transactions
        s,c=pair(new,'VoltageVector_SinCosWrapped',angle)
        assert math.isnan(s) and math.isnan(c) and new.transactions==count
        assert not new.call('VoltageVector_Compute',new.TEXT,floats=(angle,0,0,24,3,.05))
    assert not new.arguments and not new.results
    from test_foc_trig import test as sharing_test
    sharing_test(reference, candidate, factory=CordicFirmware,
                 pair_tolerance=5e-6, duty_tolerance=3e-6)
    from test_encoder_h2 import test as h2_test
    h2_test(candidate, factory=CordicFirmware)
    print(f'PASS ideal-model protocol/conversion/duty/PRIMASK: {candidate}; '
          f'pair_error={pair_error:.9g}, duty_error={duty_error:.9g}')


if __name__ == '__main__':
    for candidate in sys.argv[2:]:
        test(sys.argv[1],candidate)
