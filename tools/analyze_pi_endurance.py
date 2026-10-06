"""Summarize a single-target PI endurance log, including time-window drift."""
import argparse
import json
import math
import re
import statistics as st
from pathlib import Path


def analyze(path):
    text = Path(path).read_text(encoding='utf-8-sig')
    target = float(re.search(r'# COMMAND foc current 0 ([\d.]+)', text)[1])
    # Host markers can fall between two fragments of one UART line. Remove
    # the inserted marker and its newline without adding a separator.
    clean = re.sub(r'# (?:COMMAND [^\r\n]*|STOP_REQUEST [^\r\n]*)\r?\n', '', text)
    # A newly opened serial port may contain telemetry from an earlier run.
    telemetry = clean.rsplit('ADC DMA logger started:', 1)[1].split('\n', 1)[1]
    series = {key: [(float(t)/1000, float(v)) for t, v in re.findall(
        r'^>' + key + r':([\d.]+):([-\d.]+)', telemetry, re.M)] for key in ('iq_a', 'id_a')}
    end = series['iq_a'][-1][0] - .05

    def window(start, stop):
        q = [v for t, v in series['iq_a'] if start <= t < stop]
        d = [v for t, v in series['id_a'] if start <= t < stop]
        return dict(start_s=start, end_s=stop, samples=len(q),
                    mean_iq_a=st.mean(q), std_iq_a=st.pstdev(q),
                    rms_iq_error_a=math.sqrt(st.mean((v-target)**2 for v in q)),
                    min_iq_a=min(q), max_iq_a=max(q),
                    mean_id_a=st.mean(d), std_id_a=st.pstdev(d))

    samples = list(map(int, re.findall(r'^>sample:(\d+)', telemetry, re.M)))
    steps = [b-a for a, b in zip(samples, samples[1:])]
    aligned = ([t for t, _ in series['iq_a']] == [t for t, _ in series['id_a']]
               and len(samples) == len(series['iq_a']))
    return dict(file=str(path), target_a=target, total_iq_samples=len(series['iq_a']),
                stop_request_ms=int(re.search(r'# STOP_REQUEST elapsed_ms=(\d+)', text)[1]),
                adc_last_s=series['iq_a'][-1][0], steady=window(1, end),
                windows=[window(max(1, start), min(start+5, end))
                         for start in range(0, int(end), 5) if max(1, start) < end],
                peak_phase_a=max(map(int, re.findall(r'peak=(\d+) mA', text)))/1000,
                rpm_range=[min(map(int, re.findall(r'rpm=(-?\d+)', text))),
                           max(map(int, re.findall(r'rpm=(-?\d+)', text)))],
                temperatures_c=list(map(float, re.findall(r'T=([-\d.]+) C', text))),
                vm_v=list(map(float, re.findall(r'VM=([\d.]+) V', text))),
                faults=sorted(set(re.findall(r'fault=([^\r\n]+)', text))),
                aligned_samples=aligned,
                sample_steps=sorted(set(steps)),
                overrun=max(map(int, re.findall(r'>log_overrun:(\d+)', text))),
                saturated_snapshots=sum(map(int, re.findall(r'saturated=(\d+)', text))),
                adc_control_max_us=max(map(int, re.findall(r'adc_control_max=(\d+)', text)))/1000,
                serial_status=re.findall(r'Serial RX:[^\r\n]+', text)[-1],
                stopped='PWM: stopped' in text)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log')
    parser.add_argument('--output')
    args = parser.parse_args()
    result = json.dumps(analyze(args.log), indent=2)
    if args.output:
        Path(args.output).write_text(result, encoding='utf-8')
    print(result)
