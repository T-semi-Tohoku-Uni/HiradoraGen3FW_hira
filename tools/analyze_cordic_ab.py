import json,re,sys,statistics
from pathlib import Path
p=Path(sys.argv[1]); results=[]
enc=re.compile(r'Encoder errors: spi=(\d+), parity=(\d+), sensor=(\d+), timeout=(\d+), busy_ticks=(\d+), decode_waits=(\d+),')
for label in ('A1','B1','A2','B2'):
    raw=(p/f'{label}.log').read_text(encoding='utf-8-sig')
    data=json.loads((p/f'{label}.json').read_text(encoding='utf-8-sig'))
    starts=list(re.finditer(r'FOC current start:',raw))
    assert len(starts)==2,label
    for h2,start in zip(('off','on'),starts):
        prior=list(enc.finditer(raw[:start.start()]))[-1]
        after=enc.search(raw,start.end())
        beforevals=list(map(int,prior.groups())); aftervals=list(map(int,after.groups()))
        assert beforevals[:4]==aftervals[:4]==[0]*4,label
        run=raw[start.start():after.start()]
        states=re.findall(r'FOC: running,.*rpm=(-?\d+)',run)
        assert len(states)==50,(label,h2,len(states))
        assert 'FOC stopped: user stop' in run and 'fault=none' in run
        rows=[r for r in data if r['h2']==h2]
        assert len(rows)==50
        last=rows[-1]
        results.append(dict(label=label,fw=label[0],h2=h2,ticks=last['ticks'],
          duration_to_last_poll_s=last['ticks']/20000,
          compute_us=last['compute_max_ns']/1000,adc_us=last['adc_control_max_ns']/1000,
          age_us=last['angle_age_max_ns']/1000,
          busy_delta=aftervals[4]-beforevals[4],decode_delta=aftervals[5]-beforevals[5],
          rpm_min=min(map(int,states)),rpm_max=max(map(int,states)),
          peak_ma=max(map(int,re.findall(r'peak=(\d+) mA',run))),
          temperature_range_c=[min(map(float,re.findall(r'T=([-\d.]+) C',raw))),max(map(float,re.findall(r'T=([-\d.]+) C',raw)))],
          vm_range_v=[min(map(float,re.findall(r'VM=([\d.]+) V',raw))),max(map(float,re.findall(r'VM=([\d.]+) V',raw)))]))
    assert 'Serial RX: errors=0, overrun=0, queue_drops=0' in raw
summary={}
for h2 in ('off','on'):
    summary[h2]={}
    for fw in ('A','B'):
        runs=[r for r in results if r['h2']==h2 and r['fw']==fw]
        summary[h2][fw]={key:dict(min=min(r[key] for r in runs),median=statistics.median(r[key] for r in runs),max=max(r[key] for r in runs)) for key in ('compute_us','adc_us','age_us','busy_delta','decode_delta')}
    a=summary[h2]['A']['adc_us']['median'];b=summary[h2]['B']['adc_us']['median']
    summary[h2]['adc_median_reduction_us']=a-b
    summary[h2]['adc_median_reduction_percent']=100*(a-b)/a
(p/'summary.json').write_text(json.dumps(dict(runs=results,summary=summary),indent=2),encoding='utf-8')
print(json.dumps(dict(runs=results,summary=summary),indent=2))

