"""Summarize complete ADC intervals from an `angle trace dump` log.

Durations include trace overhead; this is not a cycle-accurate cost model.
Usage: python tools/analyze_adc_trace.py build/encoder_adc_sections_retry_2026-10-07.log
"""
import re
import statistics
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding='utf-8-sig')
clock = int(re.search(r'TRACE count=\d+ clock=(\d+)', text)[1])
events = [(int(t), int(e)) for t, e in re.findall(r'^TRACE (\d+) (\d+)', text, re.M)]
order = [3, 10, 11, 12, 13, 14, 15, 16, 17, 4]
labels = ['ADC entry/raw read', 'Current conversion/rails', 'Current protection',
          'Encoder sample/age check', 'Clarke/angle/Park', 'Logger/control entry',
          'FOC checks/speed/ramp', 'Voltage output', 'CCR/deadline/ADC exit']
frames = []
frame = []
for tick, event in events:
    if event == 3:
        frame = [(tick, event)]
    elif frame:
        frame.append((tick, event))
        if event == 4:
            if [e for _, e in frame] == order:
                frames.append([t for t, _ in frame])
            frame = []
if not frames:
    raise SystemExit('No complete instrumented ADC frames')
print(f'Complete ADC frames: {len(frames)}; trace span {(events[-1][0]-events[0][0])*1e6/clock:.3f} us')
for i, label in enumerate(labels + ['ADC total']):
    values = [((f[i+1]-f[i]) if i < len(labels) else (f[-1]-f[0]))*1e6/clock for f in frames]
    print(f'{label}: min={min(values):.3f}, median={statistics.median(values):.3f}, max={max(values):.3f} us')
