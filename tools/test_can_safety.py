"""アーム・期限・再起動抑止とUART制御排他を実ELFで検証。"""
import struct
import sys
from elftools.elf.elffile import ELFFile
from test_current_pi import Firmware
from unicorn.arm_const import UC_ARM_REG_R2, UC_ARM_REG_R3


def test(path):
    f = Firmware(path)
    def state():
        return struct.unpack('<IIIi', f.uc.mem_read(f.STATE, 16))
    def arm(now):
        return f.call('CanSafety_Arm', f.STATE, now)
    def receive(raw, received, now):
        f.uc.reg_write(UC_ARM_REG_R2, received & 0xFFFFFFFF)
        f.uc.reg_write(UC_ARM_REG_R3, now & 0xFFFFFFFF)
        return f.call('CanSafety_Receive', f.STATE, raw & 0xFFFFFFFF)
    f.uc.mem_write(f.STATE, bytes(16))
    assert not receive(16384, 100, 100) and state()[0] == 0
    assert arm(100)
    assert not receive(100, 101, 102) and state()[0] == 1
    assert not arm(120) and state()[2] == 100
    assert receive(0, 120, 121) and state() == (2, 0, 120, 0)
    assert receive(-16384, 130, 132) and state()[3] == -16384
    f.call('CanSafety_Tick', f.STATE, 179)
    assert state()[0] == 2
    f.call('CanSafety_Tick', f.STATE, 180)
    assert state()[0:2] == (3, 1) and state()[3] == 0
    assert not receive(0, 181, 181) and state()[0] == 3
    assert arm(200)
    assert not receive(0, 250, 250) and state()[0:2] == (3, 1)
    assert arm(300)
    assert not receive(0, 301, 307) and state()[0:2] == (3, 4)
    for reason in (2, 3, 5, 6):
        assert arm(400)
        f.call('CanSafety_Trip', f.STATE, reason)
        assert state()[0:2] == (3, reason) and state()[3] == 0
        assert not receive(0, 401, 401)
    f.call('CanSafety_Disarm', f.STATE)
    assert state()[0:2] == (0, 0)
    assert arm(0xFFFFFFF0)
    assert receive(0, 0xFFFFFFF5, 0xFFFFFFF6)
    f.call('CanSafety_Tick', f.STATE, 0x26)
    assert state()[0] == 2
    f.call('CanSafety_Tick', f.STATE, 0x27)
    assert state()[0:2] == (3, 1)
    # main入口の所有者ガード。ロックアウト中も制御コマンドを通さない。
    local = {}
    with open(path, 'rb') as stream:
        source = ''
        for sym in ELFFile(stream).get_section_by_name('.symtab').iter_symbols():
            if sym['st_info']['type'] == 'STT_FILE': source = sym.name
            if source == 'can_comm.c': local[sym.name] = sym['st_value']
    def command(text):
        f.uc.mem_write(f.TEXT, text.encode() + b'\0')
        return f.call('CanComm_ProcessCommand', f.TEXT)
    for mode in (1, 2, 3):
        f.uc.mem_write(local['safety'], struct.pack('<IIIi', mode, 0, 0, 0))
        for text in ('start', ' RUN CW 100 ', '1', 'u 5', 'foc current 0 1',
                     'cal start', 'cal save', 'fault clear', 'adc', 'foc h2 on'):
            assert command(text), text
        for text in ('status', 'foc status', 'angle status', 'cal status', 'vm', 'fault'):
            assert not command(text), text
    assert not command('STOP')
    assert struct.unpack('<I', f.uc.mem_read(local['safety'], 4))[0] == 0
    assert not command('start')  # 非所有時は従来UARTへ委譲するだけ（このテストで駆動しない）。
    print('PASS CAN safety:', path)


if __name__ == '__main__':
    for path in sys.argv[1:]: test(path)
