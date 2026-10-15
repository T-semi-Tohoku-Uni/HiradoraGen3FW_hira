"""Iq/基板温度APIの停止・鮮度・異常値を実ELFで検証。"""
import math
import struct
import sys
from elftools.elf.elffile import ELFFile
from test_current_pi import Firmware


def test(path):
    f = Firmware(path)
    ntc = {}
    with open(path, 'rb') as stream:
        source = ''
        for sym in ELFFile(stream).get_section_by_name('.symtab').iter_symbols():
            if sym['st_info']['type'] == 'STT_FILE':
                source = sym.name
            if source == 'ntc.c':
                ntc[sym.name] = sym['st_value']
    def put(address, fmt, value):
        f.uc.mem_write(address, struct.pack(fmt, value))
    put(f.symbols['uwTick'], '<I', 1000)
    for name in ('active', 'running', 'observation_valid'):
        put(f.foc[name], '<B', 1)
    put(f.foc['fault'], '<I', 0)
    put(f.foc['current_tick'], '<I', 999)
    put(f.foc['measured_q'], '<f', -3.5)
    assert f.call('FocVoltage_GetIqSample', f.TEXT)
    assert struct.unpack('<f', f.uc.mem_read(f.TEXT, 4))[0] == -3.5
    for name in ('active', 'running', 'observation_valid'):
        put(f.foc[name], '<B', 0)
        assert not f.call('FocVoltage_GetIqSample', f.TEXT)
        put(f.foc[name], '<B', 1)
    put(f.foc['fault'], '<I', 1)
    assert not f.call('FocVoltage_GetIqSample', f.TEXT)
    put(f.foc['fault'], '<I', 0)
    put(f.foc['current_tick'], '<I', 997)
    assert not f.call('FocVoltage_GetIqSample', f.TEXT)
    put(f.foc['current_tick'], '<I', 1000)
    put(f.foc['measured_q'], '<f', math.nan)
    assert not f.call('FocVoltage_GetIqSample', f.TEXT)
    assert not f.call('FocVoltage_GetIqSample', 0)
    put(ntc['latest_valid'], '<B', 1)
    put(ntc['latest_raw'], '<H', 3348)
    put(ntc['latest_ms'], '<I', 1000)
    assert f.call('NTC_GetTemperature', f.TEXT)
    assert 33.2 < struct.unpack('<f', f.uc.mem_read(f.TEXT, 4))[0] < 33.4
    put(ntc['latest_ms'], '<I', 899)
    assert not f.call('NTC_GetTemperature', f.TEXT)
    put(ntc['latest_ms'], '<I', 1000)
    put(ntc['latest_valid'], '<B', 0)
    assert not f.call('NTC_GetTemperature', f.TEXT)
    assert not f.call('NTC_GetTemperature', 0)
    print('PASS CAN sensor samples:', path)


if __name__ == '__main__':
    for path in sys.argv[1:]:
        test(path)
