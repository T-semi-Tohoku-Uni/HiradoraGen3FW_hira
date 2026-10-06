"""Summarize 100 Hz tuning logs; times are ADC acquisition times in ms."""
import json
import re
import statistics as st
import sys
from pathlib import Path


def analyze(path):
    text = Path(path).read_text(encoding='utf-8-sig')
    if len(re.findall(r'# COMMAND foc current ', text)) > 1:
        raise ValueError('Use analyze_transitions for a multi-command log')
    return summarize(text, path)


def summarize(text, path):
    target = float(re.search(r'# COMMAND foc current 0 ([\d.]+)', text)[1])
    data = {k: [(float(t), float(v)) for t, v in re.findall(
        r'^>' + k + r':([\d.]+):([-\d.]+)', text, re.M)] for k in ['id_a', 'iq_a']}
    q = data['iq_a']
    end = q[-1][0]
    tail = [v for t, v in q if end - 500 <= t <= end - 50]
    d = [v for t, v in data['id_a'] if end - 500 <= t <= end - 50]
    # Require five consecutive samples above 90%, to reject a single noise peak.
    t90 = next((q[i][0] for i in range(len(q)-4)
                if all(v >= .9*target for _, v in q[i:i+5])), None)
    return dict(file=str(path), target_a=target, samples=len(q),
                t90_ms=t90, mean_iq_a=st.mean(tail), std_iq_a=st.pstdev(tail),
                mean_id_a=st.mean(d), std_id_a=st.pstdev(d),
                max_iq_a=max(v for _, v in q),
                overrun=max(map(int,re.findall(r'>log_overrun:(\d+)',text)),default=0),
                peak_phase_ma=max(map(int,re.findall(r'peak=(\d+) mA',text)),default=0))


def analyze_transitions(path):
    text = Path(path).read_text(encoding='utf-8-sig')
    stages = re.split(r'(?=# COMMAND foc current )', text)[1:]
    results = [summarize(stage, f'{path}:stage{i+1}') for i, stage in enumerate(stages)]
    for result in results[1:]:
        # UART command markers are not synchronized to ADC acquisition.
        result.pop('t90_ms')
    return results


if __name__ == '__main__':
    print(json.dumps([analyze(p) for p in sys.argv[1:]], indent=2))
