"""Summarize completed signed-current H2 runs without treating noise as improvement."""
import json
from pathlib import Path

results=[]
for path in sorted(Path('build').glob('h2_matrix_r*_*_20261006.json')):
    r=json.loads(path.read_text())
    h=r['h2_steady']
    assert r['aligned_samples'] and r['sample_steps']==[200] and r['overrun']==0
    assert r['stopped'] and set(r['faults'])=={'none','user stop'}
    assert abs(h['revolutions'])>=3
    assert r['target_a']*h['mean_rpm']>0
    results.append(dict(file=str(path),round=int(path.stem.split('_')[2][1:]),
                        iq=r['target_a'],enabled=r['h2_enabled'],id2_ma=1000*h['id_h2']['amplitude'],
                        a_ma=1000*h['id_h2']['a'],b_ma=1000*h['id_h2']['b'],rpm=h['mean_rpm'],
                        iq_mean=h['mean_iq'],peak=r['peak_phase_a'],samples=r['total_iq_samples'],
                        temp=r['temperatures_c'],adc_max_us=r['adc_control_max_us'],
                        angle_error=h['correction_error_max'],wrap_error=h['wrap_error_max']))
Path('build/h2_matrix_summary_20261006.json').write_text(json.dumps(results,indent=2))
for r in results:
    print(f"r{r['round']} Iq={r['iq']:+g} H2={'on' if r['enabled'] else 'off'} "
          f"Id2={r['id2_ma']:.3f} mA rpm={r['rpm']:.1f} peak={r['peak']:.3f} "
          f"T={r['temp']} adc={r['adc_max_us']:.3f} us")
