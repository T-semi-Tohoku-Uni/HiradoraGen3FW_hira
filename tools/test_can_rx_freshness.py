"""HAL受信をモデル化し、実callbackの古い指令/FIFO異常/アーム前フレーム拒否を検証。"""
import struct
import sys
from elftools.elf.elffile import ELFFile
from test_current_pi import Firmware
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_PC, UC_ARM_REG_LR


def test(path):
    f = Firmware(path)
    local = {}
    with open(path, 'rb') as stream:
        source = ''
        for sym in ELFFile(stream).get_section_by_name('.symtab').iter_symbols():
            if sym['st_info']['type'] == 'STT_FILE': source = sym.name
            if source == 'can_comm.c': local[sym.name] = sym['st_value']
    handle = f.symbols['hfdcan1']
    f.uc.mem_write(local['can'], struct.pack('<I', handle))
    f.uc.mem_write(f.symbols['uwTick'], struct.pack('<I', 1000))
    model = {}
    def hook(uc, address, size, data):
        result = 0
        if address == (f.symbols['HAL_FDCAN_GetRxFifoFillLevel'] & ~1): result = model['remaining']
        elif address == (f.symbols['HAL_FDCAN_GetTimestampCounter'] & ~1): result = model['now']
        else:
            header = [0x200, 0, 0, 8, 0, 0, 0, model['rx'], 0, 0]
            uc.mem_write(uc.reg_read(UC_ARM_REG_R2), struct.pack('<10I', *header))
            uc.mem_write(uc.reg_read(UC_ARM_REG_R3), bytes(8))
            model['remaining'] -= 1
        uc.reg_write(UC_ARM_REG_R0, result)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    for name in ('HAL_FDCAN_GetRxFifoFillLevel', 'HAL_FDCAN_GetTimestampCounter', 'HAL_FDCAN_GetRxMessage'):
        address = f.symbols[name] & ~1
        f.uc.hook_add(UC_HOOK_CODE, hook, begin=address, end=address)
    def run(now, rx, empty=999, arm=800, flags=1):
        model.update(now=now, rx=rx, remaining=1)
        f.uc.mem_write(local['safety'], struct.pack('<IIIi', 1, 0, 995, 0))
        f.uc.mem_write(local['fifo_empty_ms'], struct.pack('<I', empty))
        f.uc.mem_write(local['arm_timestamp'], struct.pack('<H', arm))
        f.call('HAL_FDCAN_RxFifo0Callback', handle, flags)
        return struct.unpack('<IIIi', f.uc.mem_read(local['safety'], 16))
    assert run(1000, 990)[0] == 2                       # 新しいゼロ
    assert run(1000, 600)[0:2] == (3, 4)               # 6.4ms遅延
    assert run(1000, 990, empty=0)[0:2] == (3, 4)      # 一周以上の曖昧なtimestamp
    assert run(1000, 790)[0] == 1                     # アーム以前
    assert run(1000, 800)[0] == 1                     # 同じtimestampも保守的に拒否
    assert run(20, 65530, arm=65500)[0] == 2           # 正常な16bit周回
    assert run(1000, 990, flags=4)[0:2] == (3, 2)      # STM32G431 RF0L=bit2
    print('PASS CAN RX freshness:', path)


if __name__ == '__main__':
    for path in sys.argv[1:]: test(path)
