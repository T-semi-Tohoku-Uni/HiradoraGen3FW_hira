"""Summarize the saved 50 us evaluation runs; telemetry is sampled at 100 Hz."""
import json
import re
import statistics
from pathlib import Path

result = {}
for path in sorted(Path('build').glob('50us_boot*_*.log')):
    if path.stem.endswith(('_console', '_trace')):
        continue
    text = path.read_text(encoding='utf-8-sig')
    text = re.sub(r'# COMMAND [^\n]*\n', '', text)
    row = {}
    for field in ('angle_age_us', 'angle_rx_age_us'):
        values = sorted(map(float, re.findall('>' + field + r':[^:\n]+:([-\d.]+)', text)))
        if values:
            row[field] = dict(count=len(values), min=min(values), median=statistics.median(values),
                              p99=values[int(.99*(len(values)-1))], max=max(values))
    for prefix in ('FOC timing:', 'FOC current:', 'Encoder DMA:', 'Encoder errors:',
                   'Serial RX:', 'ADC DMA logger stopped:', 'PWM:'):
        matches = re.findall(re.escape(prefix) + r'[^\n]+', text)
        if matches:
            row[prefix] = matches[-1]
    samples = list(map(int, re.findall(r'>sample:(\d+)', text)))
    row['sample_increments'] = sorted(set(b-a for a, b in zip(samples, samples[1:])))
    result[path.stem] = row
print(json.dumps(result, indent=2))
