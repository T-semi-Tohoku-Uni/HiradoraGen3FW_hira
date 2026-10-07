"""Summarize the saved legacy/circular A/B logs (run from repository root).

IRQ occupancy is the union of TIM1/ADC/RX trace intervals, excluding double
counting of preemption. It is not total CPU utilization. Trace windows cover
startup only; 100 Hz telemetry is not an exhaustive 20 kHz observation.
"""
import json
import re
import statistics
from pathlib import Path

result = {}
for mode in ('legacy', 'circular'):
    log = Path(f'build/ab_{mode}_trace.log').read_text(encoding='utf-8-sig')
    clock = int(re.search(r'TRACE count=\d+ clock=(\d+)', log)[1])
    to_us = 1e6 / clock
    events = [tuple(map(int, x)) for x in re.findall(r'^TRACE (\d+) (\d+)', log, re.M)]
    summary = {}
    for first, last, name in ((1, 2, 'TIM1'), (3, 4, 'ADC'), (5, 6, 'RX')):
        start = None
        durations = []
        for tick, event in events:
            if event == first:
                start = tick
            elif event == last and start is not None:
                durations.append((tick - start) * to_us)
                start = None
        summary[name] = {'count': len(durations), 'median_us': statistics.median(durations),
                         'max_us': max(durations)}
    starts = [t for t, e in events if e == 1]
    lo, hi = starts[1], starts[-1]
    stack, previous, occupied = [], None, 0
    for tick, event in events:
        if previous is not None and stack:
            occupied += max(0, min(tick, hi) - max(previous, lo))
        if event in (1, 3, 5):
            stack.append(event)
        elif event in (2, 4, 6):
            if not stack or stack.pop() != event - 1:
                raise ValueError('Unmatched IRQ trace')
        previous = tick
    summary['complete_pwm_cycles'] = len(starts) - 2
    summary['irq_union_us_per_pwm_cycle'] = occupied * to_us / (len(starts) - 2)
    publish = [t for t, e in events if e == 9]
    intervals = [(b-a)*to_us for a, b in zip(publish, publish[1:])]
    summary['publish_interval_us'] = [min(intervals), statistics.median(intervals), max(intervals)]
    result[mode] = {'trace': summary}
    for direction in ('pos', 'neg'):
        log = Path(f'build/ab_{mode}_{direction}_adc.log').read_text(encoding='utf-8-sig')
        log = re.sub(r'# COMMAND [^\n]*\n', '', log)
        data = {}
        for name in ('angle_age_us', 'angle_rx_age_us'):
            values = sorted(float(x) for x in re.findall('>' + name + r':[^:\n]+:([-\d.]+)', log))
            data[name] = {'count': len(values), 'min': min(values), 'median': statistics.median(values),
                          'p99': values[int(.99*(len(values)-1))], 'max': max(values)}
        for prefix in ('FOC timing:', 'FOC current:', 'Encoder errors:', 'Serial RX:', 'ADC DMA logger stopped:'):
            data[prefix] = re.findall(re.escape(prefix) + r'[^\n]+', log)[-1]
        samples = [int(x) for x in re.findall(r'>sample:(\d+)', log)]
        data['sample_increments'] = sorted(set(b-a for a, b in zip(samples, samples[1:])))
        result[mode][direction] = data
print(json.dumps(result, indent=2))
