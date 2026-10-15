"""CAN→FOC接続とTIM監視を実ELFで検証。起動/センサーはモデルに置換。"""
import struct
import sys
from elftools.elf.elffile import ELFFile
from test_current_pi import Firmware, close
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_LR


def test(path):
    f = Firmware(path)
    local = {}
    with open(path, 'rb') as stream:
        source = ''
        for sym in ELFFile(stream).get_section_by_name('.symtab').iter_symbols():
            if sym['st_info']['type'] == 'STT_FILE': source = sym.name
            if source == 'can_comm.c': local[sym.name] = sym['st_value']
    def put(addr, value):
        f.uc.mem_write(addr, struct.pack('<I', value & 0xFFFFFFFF))
    def state(mode=2, raw=0):
        f.uc.mem_write(local['safety'], struct.pack('<IIIi', mode, 0, 1000, raw))
        put(local['control_cycles'], 100)
        put(0xE0001004, 100)
    def read_state():
        return struct.unpack('<IIIi', f.uc.mem_read(local['safety'], 16))
    put(f.symbols['SystemCoreClock'], 160000000)
    put(f.symbols['uwTick'], 1000)
    f.uc.mem_write(local['ready'], b'\1')
    put(local['tx_ms'], 1000)  # 通信送信はこのモデルの対象外。
    calls = []
    start_ok = True
    def stub(uc, address, size, data):
        result = 0
        if address == (f.symbols['Foc_StartCurrent'] & ~1):
            calls.append('start')
            put(f.foc['control_permitted'], uc.reg_read(UC_ARM_REG_R0))
            f.uc.mem_write(f.foc['active'], bytes([int(start_ok)]))
            put(f.foc['control_mode'], 1)
            result = int(start_ok)
        elif address == (f.symbols['HAL_GPIO_ReadPin'] & ~1): result = 1
        uc.reg_write(UC_ARM_REG_R0, result)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    for name in ('Foc_StartCurrent', 'HAL_GPIO_ReadPin', 'AS5047P_GetSample',
                 'HAL_FDCAN_GetRxFifoFillLevel'):
        address = f.symbols[name] & ~1
        f.uc.hook_add(UC_HOOK_CODE, stub, begin=address, end=address)
    for mode in (0, 1, 3):
        state(mode)
        f.call('CanComm_Task')
        assert not calls  # ゼロhandshake前には開始しない。
    state()
    f.call('CanComm_Task')
    assert calls == ['start']
    for raw, expected in ((8192, 10), (16384, 15), (-32768, -15), (0, 0)):
        state(raw=raw)
        f.call('CanComm_Task')
        close(f.get('current_target_d', '<f'), 0)
        close(f.get('current_target_q', '<f'), expected)
        assert calls == ['start']
    # SysTickを進めず、DWTだけ50ms進めてTIM側から失効させる。
    state(raw=8192)
    put(0xE0001004, 100 + 8000000)
    f.call('FocVoltage_WatchdogISR')
    assert read_state()[0:2] == (3, 1)
    assert not f.call('Foc_IsHealthy')
    f.call('CanComm_Task')
    assert calls == ['start']
    # 起動失敗はFAULTをラッチし、次のmainで再起動しない。
    start_ok = False
    f.uc.mem_write(local['foc_requested'], b'\0')
    f.uc.mem_write(f.foc['active'], b'\0')
    put(f.foc['fault'], 0)
    state()
    f.call('CanComm_Task')
    assert read_state()[0:2] == (3, 7)
    f.call('CanComm_Task')
    assert len(calls) == 2
    # DWT周回を跨いだ期限も、受信再開で自動解除しない。
    state()
    put(local['control_cycles'], 0xFFFFFF00)
    put(0xE0001004, (0xFFFFFF00 + 8000000) & 0xFFFFFFFF)
    f.call('CanComm_WatchdogISR')
    assert read_state()[0:2] == (3, 1)
    print('PASS CAN FOC clamp, start-once, TIM/DWT timeout, start failure:', path)


if __name__ == '__main__':
    for path in sys.argv[1:]: test(path)
