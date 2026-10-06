"""Fit Id versus raw mechanical angle, and verify H2 formula in time windows."""
import argparse
import json
import math
import re
import statistics
from pathlib import Path
from analyze_pi_endurance import analyze


def fit(rows, key):
    m = [[0.0]*4 for _ in range(3)]
    for row in rows:
        t = row['mech_raw_rad']
        x = [1, math.cos(2*t), math.sin(2*t)]
        for i in range(3):
            for j in range(3):
                m[i][j] += x[i]*x[j]
            m[i][3] += x[i]*row[key]
    for i in range(3):
        pivot = max(range(i, 3), key=lambda j: abs(m[j][i]))
        m[i], m[pivot] = m[pivot], m[i]
        assert abs(m[i][i]) > 1e-8, 'Insufficient angular coverage'
        divisor = m[i][i]
        m[i] = [v/divisor for v in m[i]]
        for j in range(3):
            if j != i:
                scale = m[j][i]
                m[j] = [a-scale*b for a, b in zip(m[j], m[i])]
    c, a, b = [m[i][3] for i in range(3)]
    return dict(c=c, a=a, b=b, amplitude=math.hypot(a,b), phase_deg=math.degrees(math.atan2(b,a)))


def inspect(path, enabled):
    summary = analyze(path)
    text = Path(path).read_text(encoding='utf-8-sig')
    text = re.sub(r'# (?:COMMAND [^\r\n]*|STOP_REQUEST [^\r\n]*)\r?\n', '', text)
    a2,b2 = map(float,re.search(r'Calibration H2: a2=([-\d.]+) b2=([-\d.]+)',text).groups())
    telemetry = text.rsplit('ADC DMA logger started:',1)[1]
    keys = ['id_a','iq_a','mech_raw_rad','elec_raw_rad','elec_base_rad','elec_rad','elec_corr_rad']
    rows = {}
    for key,t,value in re.findall(r'^>('+'|'.join(keys)+r'):([\d.]+):([-\d.]+)',telemetry,re.M):
        rows.setdefault(float(t)/1000,{})[key] = float(value)
    assert all(len(v)==len(keys) for v in rows.values()), 'Incomplete angle/current samples'
    assert len(rows)==summary['total_iq_samples']
    def window(start,end):
        selected = [r for t,r in rows.items() if start<=t<end]
        assert len(selected)>100
        corr = [abs(r['elec_corr_rad']-(-(a2*math.cos(2*r['mech_raw_rad'])+b2*math.sin(2*r['mech_raw_rad'])) if enabled else 0)) for r in selected]
        wrap = [abs(math.remainder(r['elec_rad']-r['elec_base_rad']-r['elec_corr_rad'],math.tau)) for r in selected]
        travel = sum(math.remainder(b['mech_raw_rad']-a['mech_raw_rad'],math.tau) for a,b in zip(selected,selected[1:]))
        return dict(start_s=start,end_s=end,samples=len(selected),revolutions=travel/math.tau,
                    mean_rpm=travel/math.tau*60/(end-start),id_h2=fit(selected,'id_a'),
                    iq_h2=fit(selected,'iq_a'),mean_iq=statistics.mean(r['iq_a'] for r in selected),
                    correction_error_max=corr and max(corr),wrap_error_max=max(wrap))
    end=min(29,summary['adc_last_s']-.05)
    summary['h2_enabled']=enabled
    summary['h2_steady']=window(2,end)
    summary['h2_windows']=[window(lo,min(hi,end)) for lo,hi in [(2,10),(10,20),(20,end)] if min(hi,end)-lo>1.1]
    assert summary['h2_steady']['correction_error_max']<0.0007
    assert summary['h2_steady']['wrap_error_max']<0.0016
    return summary


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('log');p.add_argument('--enabled',action='store_true');p.add_argument('--output',required=True)
    args=p.parse_args()
    result=inspect(args.log,args.enabled)
    Path(args.output).write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2))
