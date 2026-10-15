"""実ELFのCANパック/アンパックをUnicornで検証（実機通信は対象外）。"""
import struct
import sys
from test_current_pi import Firmware
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC


def test(path):
    f = Firmware(path)
    data, output, ident = 0x20011000, 0x20011100, 0x20011200

    def call(name, *args):
        sp = 0x2001F000
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            f.uc.reg_write(reg, value & 0xFFFFFFFF)
        if len(args) > 4:
            f.uc.mem_write(sp, struct.pack('<' + 'I' * (len(args) - 4),
                                        *(v & 0xFFFFFFFF for v in args[4:])))
        f.uc.reg_write(UC_ARM_REG_SP, sp)
        f.uc.reg_write(UC_ARM_REG_LR, f.RETURN | 1)
        f.uc.emu_start(f.symbols[name] | 1, f.RETURN, count=10000)
        assert f.uc.reg_read(UC_ARM_REG_PC) == f.RETURN
        return f.uc.reg_read(UC_ARM_REG_R0)

    for node in range(1, 9):
        frame_id = 0x200 if node <= 4 else 0x1FF
        for raw in (-32768, -16384, -1, 0, 1, 16384, 32767):
            payload = bytearray(b'\x12\x34' * 4)
            offset = ((node - 1) % 4) * 2
            payload[offset:offset + 2] = struct.pack('>h', raw)
            f.uc.mem_write(data, bytes(payload))
            assert call('CanProtocol_Decode', node, frame_id, data, 8, output)
            assert struct.unpack('<h', f.uc.mem_read(output, 2))[0] == raw
        for wrong_id in (0x201, 0x1FF if node <= 4 else 0x200, 0x1200):
            assert not call('CanProtocol_Decode', node, wrong_id, data, 8, output)
        for length in (0, 7, 9, 64):
            assert not call('CanProtocol_Decode', node, frame_id, data, length, output)
        assert call('CanProtocol_Pack', node, 8191, -1234, -16384, 42, ident, output)
        assert struct.unpack('<I', f.uc.mem_read(ident, 4))[0] == 0x200 + node
        assert bytes(f.uc.mem_read(output, 8)) == bytes.fromhex('1f ff fb 2e c0 00 2a 00')
    for node in (0, 9, 255):
        assert not call('CanProtocol_Decode', node, 0x200, data, 8, output)
        assert not call('CanProtocol_Pack', node, 0, 0, 0, 0, ident, output)
    assert not call('CanProtocol_Decode', 1, 0x200, 0, 8, output)
    assert not call('CanProtocol_Decode', 1, 0x200, data, 8, 0)
    assert not call('CanProtocol_Pack', 1, 8192, 0, 0, 0, ident, output)
    assert not call('CanProtocol_Pack', 1, 0, 0, 0, 0, 0, output)
    assert not call('CanProtocol_Pack', 1, 0, 0, 0, 0, ident, 0)
    print('PASS CAN protocol:', path)


if __name__ == '__main__':
    for path in sys.argv[1:]:
        test(path)
