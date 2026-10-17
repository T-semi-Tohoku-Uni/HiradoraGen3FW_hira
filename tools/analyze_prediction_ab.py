"""補正ON/OFFログの定常区間を集計する（入力電力・軸トルクは未計測）。"""
import json
import math
import re
import statistics as st
import sys
from pathlib import Path

folder = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/prediction_ab_20261017')
rows = []
for p in sorted(folder.glob('[AB][12]_*.log')):
    if p.stem.endswith('_console'):
        continue
    s = p.read_text(encoding='utf-8-sig')
    frames = {}
    for k, t, v in re.findall(r'^>(\w+):(\d+\.\d{3}):(-?\d+\.\d{3})\r?$', s, re.M):
        frames.setdefault(float(t), {})[k] = float(v)
    required = {'id_a','iq_a','u1_a','u2_a','v_a','w_a','mech_raw_rad','elec_rad','pwm_elec_rad','omega_e_rad_s','angle_pwm_dt_us'}
    fs = [(t, v) for t, v in sorted(frames.items()) if t >= 1000 and required <= v.keys()]
    if not fs:
        continue
    speeds = []
    for (ta, a), (tb, b) in zip(fs, fs[1:]):
        delta = (b['mech_raw_rad'] - a['mech_raw_rad'] + math.pi) % (2*math.pi) - math.pi
        speeds.append(delta / ((tb-ta)*1e-3) * 60/(2*math.pi))
    # Status records are asynchronous; retain them separately from ADC samples.
    voltages = []
    latest_t = 0
    for line in s.splitlines():
        m = re.match(r'>iq_a:(\d+\.\d+):', line)
        if m:
            latest_t = float(m[1])
        m = re.search(r'FOC: running.*applied=(-?\d+)/(-?\d+) mV.*rpm=(-?\d+)', line)
        if m and latest_t >= 1000:
            vd, vq, rpm = map(int, m.groups())
            voltages.append({'voltage_v': math.hypot(vd,vq)/1000, 'rpm': rpm})
    vals = [v for _, v in fs]
    rms = math.sqrt(st.mean((((v['u1_a']+v['u2_a'])/2)**2+v['v_a']**2+v['w_a']**2)/3 for v in vals))
    row = dict(run=p.stem, mode='OFF' if p.name.startswith('A') else 'ON', samples=len(fs),
        rpm_mean=st.mean(speeds), rpm_sd=st.pstdev(speeds), phase_rms_a=rms,
        iq_mean_a=st.mean(v['iq_a'] for v in vals), iq_sd_a=st.pstdev(v['iq_a'] for v in vals),
        id_mean_a=st.mean(v['id_a'] for v in vals), id_sd_a=st.pstdev(v['id_a'] for v in vals),
        voltage_mean_v=st.mean(v['voltage_v'] for v in voltages) if voltages else None,
        voltage_samples=voltages,
        prediction_deg_mean=st.mean(abs(v['omega_e_rad_s']*v['angle_pwm_dt_us']*1e-6)*180/math.pi for v in vals),
        park_pwm_gap_max_rad=max(abs((v['pwm_elec_rad']-v['elec_rad']+math.pi)%(2*math.pi)-math.pi) for v in vals),
        adc_max_us=max(map(int,re.findall(r'adc_control_max=(\d+) ns',s)),default=0)/1000,
        temperatures_c=list(map(float,re.findall(r'T=([-\d.]+) C',s))),
        faults=sorted(set(re.findall(r'fault=([^\r\n;]+)',s))),
        log_overrun_max=max(map(int,re.findall(r'>log_overrun:(\d+)',s)),default=0),
        stopped='PWM: stopped, mode=off' in s)
    rows.append(row)
(folder/'summary.json').write_text(json.dumps(rows,indent=2),encoding='utf-8')
for r in rows:
    print(f"{r['run']:7} {r['mode']:3} n={r['samples']} rpm={r['rpm_mean']:.2f} phaseRMS={r['phase_rms_a']:.5f} IqSD={r['iq_sd_a']:.5f} IdSD={r['id_sd_a']:.5f} V={r['voltage_mean_v']:.4f} ADCmax={r['adc_max_us']:.3f} T={r['temperatures_c']} faults={r['faults']}")
