"""Summarize completed phase-alignment hardware logs (100 Hz telemetry)."""
import json
import re
import statistics
import sys
from pathlib import Path

rows = {}
for arg in sys.argv[1:]:
    p = Path(arg)
    text = re.sub(r'# COMMAND [^\n]*\n', '', p.read_text(encoding='utf-8-sig'))
    row = {}
    for field in ('angle_age_us', 'angle_rx_age_us'):
        values = sorted(map(float, re.findall('>'+field+r':[^:\n]+:([-\d.]+)', text)))
        if values:
            row[field] = dict(count=len(values), min=min(values), median=statistics.median(values),
                              p99=values[int(.99*(len(values)-1))], max=max(values))
    for prefix in ('FOC timing:', 'FOC current:', 'FOC phase:', 'FOC encoder phase:',
                   'Encoder DMA:', 'Encoder errors:', 'Serial RX:', 'ADC DMA logger stopped:', 'PWM:'):
        matches = re.findall(re.escape(prefix)+r'[^\n]+', text)
        if matches: row[prefix] = matches[-1]
    samples = list(map(int, re.findall(r'>sample:(\d+)', text)))
    row['sample_increments'] = sorted(set(b-a for a,b in zip(samples,samples[1:])))
    temps = list(map(float, re.findall(r'T=([-\d.]+) C',text)))
    if temps: row['temperature_max_C'] = max(temps)
    rows[p.stem] = row
print(json.dumps(rows, indent=2))
