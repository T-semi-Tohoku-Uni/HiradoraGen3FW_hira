"""HALの完了ビット残留・エラー履歴による二重集計の回帰テスト。"""
import struct
import sys
from elftools.elf.elffile import ELFFile
from test_current_pi import Firmware


def test(path):
    f = Firmware(path)
    symbols = {}
    with open(path, 'rb') as stream:
        source = ''
        for sym in ELFFile(stream).get_section_by_name('.symtab').iter_symbols():
            if sym['st_info']['type'] == 'STT_FILE':
                source = sym.name
            if source == 'can_comm.c':
                symbols[sym.name] = sym['st_value']

    def put(name, value):
        f.uc.mem_write(symbols[name], struct.pack('<I', value))

    def counter(offset):
        return struct.unpack('<I', f.uc.mem_read(symbols['stats'] + offset, 4))[0]

    handle = f.symbols['hfdcan1']
    put('can', handle)
    # 過去の完了ビットを含む7でも、今回要求したバッファだけ集計。
    for index, bit in enumerate((1, 2, 4, 1), 1):
        put('awaiting_completion', bit)
        f.call('HAL_FDCAN_TxBufferCompleteCallback', handle, 7)
        assert counter(20) == index
        f.call('HAL_FDCAN_TxBufferCompleteCallback', handle, 7)
        assert counter(20) == index
    put('awaiting_completion', 2)
    f.call('HAL_FDCAN_TxBufferCompleteCallback', handle + 4, 7)
    assert counter(20) == 4
    # APIエラー履歴を注入。HAL_GetErrorの実関数が参照するErrorCodeへ設定。
    # ErrorCodeオフセットはHAL_GetErrorの実行時メモリ読出しから取得する。
    from unicorn import UC_HOOK_MEM_READ
    reads = []
    hook = f.uc.hook_add(UC_HOOK_MEM_READ, lambda uc, access, addr, size, value, data: reads.append(addr))
    f.call('HAL_FDCAN_GetError', handle)
    f.uc.hook_del(hook)
    error_address = next(addr for addr in reads if handle <= addr < handle + 512)
    f.uc.mem_map(0x40000000, 0x100000)
    f.uc.mem_write(handle, struct.pack('<I', 0x4000A400))
    f.uc.mem_write(error_address, struct.pack('<I', 0x20))
    f.call('HAL_FDCAN_ErrorCallback', handle)
    assert counter(24) == 1
    assert struct.unpack('<I', f.uc.mem_read(error_address, 4))[0] == 0
    f.call('HAL_FDCAN_ErrorCallback', handle)
    assert counter(24) == 1
    print('PASS CAN callback accounting:', path)


if __name__ == '__main__':
    for path in sys.argv[1:]:
        test(path)
