"""Run the actual ARM encoder driver with a small peripheral model (no board).

python tools/test_encoder_tim8.py build/Debug/HiradoraGen3FW.elf build/Release/HiradoraGen3FW.elf
Requires unicorn and pyelftools, like test_current_pi.py. Models DMA flags/buffers
and DWT, not electrical SPI timing, DMA arbitration, or instruction cycle counts.
"""
import math
import struct
import sys

from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_PC, UC_ARM_REG_LR
from test_current_pi import Firmware


SPI, TIM, DMA = 0x40013000, 0x40013400, 0x40020000
RX, TX = DMA + 0x1C, DMA + 0x30
DWT = 0xE0001004
MASK = 0xFFFFFFFF


def parity(word):
    return word | ((word.bit_count() & 1) << 15)


class Encoder(Firmware):
    def __init__(self, path, invalid=None):
        self.display_samples = []
        super().__init__(path)
        self.uc.mem_map(0x40000000, 0x100000)
        self.uc.mem_map(0x48000000, 0x10000)
        self.enc = {}
        with open(path, 'rb') as stream:
            source = ''
            for sym in ELFFile(stream).get_section_by_name('.symtab').iter_symbols():
                if sym['st_info']['type'] == 'STT_FILE':
                    source = sym.name
                if source == 'as5047p.c':
                    self.enc[sym.name] = sym['st_value']
        self.cycles, self.ms, self.boot = 100000, 0, True
        self.stopped = True
        self.epoch, self.frame, self.prior = 0, 0, None
        self.inject_delay = 0
        self.uc.hook_add(UC_HOOK_MEM_READ, self.read_clock, begin=DWT, end=DWT+3)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write_register, begin=DMA+4, end=DMA+7)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write_register, begin=TIM, end=TIM+3)
        for name in ('HAL_GetTick', 'HAL_Delay', 'HAL_RCC_GetPCLK2Freq', 'MotorControl_IsStopped'):
            a = self.symbols[name] & ~1
            self.uc.hook_add(UC_HOOK_CODE, self.environment, begin=a, end=a)
        self.write(self.symbols['SystemCoreClock'], 160000000)
        spi, tim = self.symbols['hspi1'], self.symbols['htim8']
        # Fixture uses this repository's HAL ABI: instance + SPI_InitTypeDef.
        self.uc.mem_write(spi, struct.pack('<14I', SPI, 0x104, 0, 0xF00,
                                           0, 1, 0x200, 0x20, 0, 0, 0, 7, 0, 0))
        # SPI prescaler /32 is BR=4, i.e. 0x20. The real MSP initializes RX DMA.
        self.call('MX_SPI1_Init') if 'MX_SPI1_Init' in self.symbols else self.call('HAL_SPI_Init', spi)
        if 'MX_TIM8_Init' in self.symbols:
            self.call('MX_TIM8_Init')  # Debug: test actual generated initialization too
        else:
            # Release inlines MX_TIM8_Init into main. Register fixture + real MSP.
            self.write(tim, TIM)
            self.call('HAL_TIM_Base_MspInit', tim)
            for offset, value in ((0, 0x80), (4, 0x100), (0x18, 0x60),
                                  (0x28, 0), (0x2C, 7999), (0x30, 0),
                                  (0x34, 6400), (0x38, 6560), (0x44, 0x400)):
                self.write(TIM+offset, value)
        if invalid == 'timer':
            self.write(TIM+0x38, 6400)
        elif invalid == 'tx_increment':
            # DMA handle: Instance, Request, Direction, PeriphInc, MemInc.
            self.write(self.symbols['hdma_tim8_ch2']+16, 0x80)
        self.call('AS5047P_Init', spi, tim)
        self.boot = False
        if invalid:
            assert not self.e('owned', '<?')
            assert not self.read(TIM) & 1
            return
        assert self.e('owned', '<?'), self.messages
        # No automatic frames are generated during Init's acquisition wait.
        # Restart cleanly through the public pause/resume path for each scenario.
        self.call('AS5047P_Pause')
        self.ms += 1
        self.call('AS5047P_Resume')
        self.call('AS5047P_Task')
        assert self.e('state') == 1, self.messages
        self.base = {n: self.e(n) for n in ('timeouts', 'overruns', 'spi_errors')}
        assert self.read(RX+4) == 2 and self.read(TX+4) == 1
        assert self.read(RX) & 0xAE == 0xAE  # circular, increment, HT/TC/TE
        assert self.read(TX) & 0xBE == 0x38  # circular M->P, TE only, no increment
        assert self.read(SPI+4) & 3 == 1  # RX DMA enabled; SPI TX DMA disabled
        assert self.read(TIM+0xC) == 0x400  # CC2DE, no timer CPU IRQ

    def stub(self, uc, address, size, data):
        if address == (self.symbols['printf'] & ~1):
            message = self.string(uc.reg_read(UC_ARM_REG_R0))
            if 'raw=%u, mech=%lu.%03lu' in message:
                fraction, electrical, electrical_fraction = struct.unpack(
                    '<3I', uc.mem_read(uc.reg_read(UC_ARM_REG_SP), 12))
                self.display_samples.append((uc.reg_read(UC_ARM_REG_R3)*1000 + fraction,
                                             electrical*1000 + electrical_fraction))
        super().stub(uc, address, size, data)

    def read(self, addr, fmt='<I'):
        return struct.unpack(fmt, self.uc.mem_read(addr, struct.calcsize(fmt)))[0]

    def write(self, addr, value, fmt='<I'):
        self.uc.mem_write(addr, struct.pack(fmt, value & MASK if fmt == '<I' else value))

    def e(self, name, fmt='<I'):
        return self.read(self.enc[name], fmt)

    def environment(self, uc, address, size, data):
        if address == (self.symbols['HAL_GetTick'] & ~1):
            if self.boot:
                self.ms += 1
            result = self.ms
        elif address == (self.symbols['HAL_RCC_GetPCLK2Freq'] & ~1):
            result = 160000000
        elif address == (self.symbols['MotorControl_IsStopped'] & ~1):
            result = int(self.stopped)
        else:
            self.ms += uc.reg_read(UC_ARM_REG_R0)
            result = 0
        uc.reg_write(UC_ARM_REG_R0, result)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def read_clock(self, uc, access, address, size, value, data):
        self.cycles = (self.cycles + 16) & MASK
        self.write(DWT, self.cycles)

    def write_register(self, uc, access, address, size, value, data):
        if address == DMA+4:
            # IFCR is write-one-to-clear. A GI bit clears the whole channel group.
            clear = value
            for shift in (0, 4, 8):
                if value & (1 << shift):
                    clear |= 15 << shift
            self.write(DMA, self.read(DMA) & ~clear)
            if self.inject_delay and value & 0xF00:
                self.cycles = (self.cycles + self.inject_delay) & MASK
                self.inject_delay = 0
        elif address == TIM and value & 1 and not self.read(TIM) & 1:
            self.epoch, self.frame, self.prior = self.cycles, 0, None

    def deliver(self, raw=1234, response=None, late=0, flags=None, tx_complete=True):
        sent = self.read(self.read(TX+12), '<H')
        assert not sent.bit_count() & 1 and sent & 0x4000
        if response is None:
            response = {None: 0xFFFF, 0x3FFC: parity(0x100),
                        0x0001: parity(1)}.get(self.prior, parity(raw))
        self.prior = sent & 0x3FFF
        self.cycles = (self.epoch + 6560 + self.frame * 8000 + 600 + late) & MASK
        self.write(TIM+0x24, (7160 + late) % 8000)
        slot = self.frame % 2
        self.write(self.read(RX+12)+slot*2, response, '<H')
        self.write(RX+4, 1 if slot == 0 else 2)
        self.write(DMA, ((0x40 if slot == 0 else 0x20) if flags is None else flags)
                   | (0x200 if tx_complete else 0))
        self.call('AS5047P_DMA_IRQHandler', self.symbols['hdma_spi1_rx'])
        self.frame += 1

    def sample(self):
        ok = self.call('AS5047P_GetSample', self.CONFIG)
        return ok, struct.unpack('<H2xffIIIIB3x', self.uc.mem_read(self.CONFIG, 32))

    def ready(self, raw=1234):
        self.deliver(raw)
        assert self.sample()[0] == 0  # first response has no associated request
        self.deliver(raw)
        assert self.sample()[0] == 0  # diagnostic is not an angle
        self.deliver(raw)
        assert self.sample()[0] == 1


def test(path):
    # Inspect actual ARM printf arguments at/above the former uint32 overflow.
    for raw in (0, 11930, 11931, 13001, 16383):
        e = Encoder(path); e.ready(raw)
        e.uc.mem_write(e.TEXT, b'angle status\0')
        e.call('AS5047P_ProcessCommand', e.TEXT)
        e.call('AS5047P_Task')
        assert e.display_samples[-1] == ((raw*360000 + 8192)//16384,
                                         ((raw*7 % 16384)*360000 + 8192)//16384)
    e = Encoder(path)
    e.ready()
    ok, s = e.sample()
    assert s[0] == 1234 and abs(s[1] - 1234*2*math.pi/16384) < 1e-6
    assert s[3] == (e.epoch + 6560 + 8000) & MASK  # previous command, not this IRQ
    for raw in (0, 16383, 0, 0, 1234):
        before = e.sample()[1][6]
        e.deliver(raw)
        ok, s = e.sample()
        assert ok and s[0] == raw and s[6] == before + 1
    before = e.sample()[1][6]
    e.ms += 20  # request periodic diagnostics; transport has continued physically
    e.write(e.enc['last_received_ms'], e.ms)
    for _ in range(5):
        e.deliver(2000)
    assert e.sample()[0] and e.sample()[1][0] == 2000
    assert e.sample()[1][6] == before + 4  # one diagnostic response, four angles

    for bad, field in ((parity(4000) ^ 0x8000, 'parity_errors'),
                       (parity(4000 | 0x4000), 'sensor_errors')):
        e = Encoder(path); e.ready()
        before = e.e(field)
        e.deliver(response=bad)
        assert not e.sample()[0] and e.e(field) == before + 1
        for _ in range(8):
            e.deliver()
        assert e.sample()[0] and e.e('error_flags', '<H') == 1

    e = Encoder(path)
    e.deliver(); e.deliver(response=0)  # MISO stuck low must not pass diagnostics
    e.deliver(response=0)
    assert not e.sample()[0]

    for kwargs in ({'late': 8000}, {'flags': 0x60}, {'tx_complete': False}):
        e = Encoder(path); e.ready(); e.deliver(**kwargs)
        assert not e.sample()[0] and e.e('overruns') == e.base['overruns'] + 1
        assert not e.read(TIM) & 1 and not e.read(TIM+0x44) & 0x8000
    e = Encoder(path); e.ready()
    e.inject_delay = 8000  # simulated higher-priority work between capture and commit
    e.deliver()
    assert not e.sample()[0] and e.e('overruns') == e.base['overruns'] + 1

    e = Encoder(path); e.ready()
    e.cycles = (e.cycles + 17000) & MASK
    assert not e.sample()[0] and e.e('timeouts') == e.base['timeouts'] + 1
    e.stopped = False; e.call('AS5047P_Task')
    assert e.e('state') == 2  # no recovery that hides a fault while motor is active
    e.stopped = True; e.call('AS5047P_Task')
    assert e.e('state') == 1
    e.ready()

    e = Encoder(path); e.ready()
    e.write(TIM, e.read(TIM) & ~1)
    assert not e.sample()[0]
    for flags, handle in ((0x80, 'hdma_spi1_rx'), (0x800, 'hdma_tim8_ch2')):
        e = Encoder(path); e.ready(); e.write(DMA, flags)
        e.call('AS5047P_DMA_IRQHandler', e.symbols[handle])
        assert not e.sample()[0] and e.e('spi_errors') == e.base['spi_errors'] + 1
    e = Encoder(path); e.ready(); e.write(SPI+8, 0x40)  # OVR
    e.call('AS5047P_SPI_IRQHandler', e.symbols['hspi1'])
    assert not e.sample()[0]

    e = Encoder(path); e.ready(); e.call('AS5047P_Pause')
    assert not e.sample()[0] and not e.read(TIM+0xC) & 0x400
    assert not e.read(RX) & 1 and not e.read(TX) & 1
    e.cycles = 0xFFFFD000  # restart across DWT rollover
    e.call('AS5047P_Resume'); e.call('AS5047P_Task')
    e.ready(16383)
    assert e.sample()[1][1] < 2*math.pi
    for _ in range(8):
        e.deliver(0)
    assert e.sample()[0]
    Encoder(path, 'timer')
    Encoder(path, 'tx_increment')
    print(f'PASS: TIM8 pipeline, diagnostics, timestamps, loss/deadline, faults, restart, wrap, config: {path}')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for elf in sys.argv[1:]:
        test(elf)
