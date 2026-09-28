"""既存Teleplotログから電流観測を集計する（標準ライブラリのみ）。
停止直前の過渡を含めないため、全区間で取得時刻の先頭・末尾各1秒を除外。
相電流はA、elec_radはrad。未記録の点は補間しない。
"""
import sys, re, math, statistics as st, json
from pathlib import Path

def stats(a):
    a=sorted(a)
    return dict(mean=st.mean(a), rms=math.sqrt(st.mean(x*x for x in a)),
                std=st.pstdev(a), min=a[0], max=a[-1], p95_abs=sorted(map(abs,a))[int(.95*(len(a)-1))])

def analyze(path, polarity=1.0, direction=1.0):
    text=path.read_text(encoding="utf-8")
    keys=["u1_a","v_a","u2_a","w_a","id_a","iq_a","elec_rad"]
    data={k:{float(t):float(v) for t,v in re.findall(r"^>"+k+r":([\d.]+):([-\d.]+)",text,re.M)} for k in keys}
    foc=bool(data["id_a"])
    used=keys if foc else keys[:4]
    times=sorted(set.intersection(*(set(data[k]) for k in used)))
    if not times: raise ValueError(f"No complete samples: {path}")
    full=len(times)
    times=[t for t in times if times[0]+1000<=t<=times[-1]-1000]
    if not times: raise ValueError("Need more than two seconds of data")
    values={k:[data[k][t] for t in times] for k in used}
    u=[(data["u1_a"][t]+data["u2_a"][t])/2 for t in times]
    values["u1_minus_u2"]=[data["u1_a"][t]-data["u2_a"][t] for t in times]
    values["phase_sum"]=[a+data["v_a"][t]+data["w_a"][t] for a,t in zip(u,times)]
    if foc:
        # 実機に書き込んだ極性・校正方向を引数で指定して照合する。
        d=[];q=[]
        for a,t in zip(u,times):
            v,w,theta=(data[k][t] for k in ["v_a","w_a","elec_rad"])
            alpha=(2*a-v-w)/3*polarity;beta=(v-w)/math.sqrt(3)*polarity
            d.append(alpha*math.cos(theta)+beta*math.sin(theta)-data["id_a"][t])
            q.append((-alpha*math.sin(theta)+beta*math.cos(theta))*direction-data["iq_a"][t])
        values["id_recompute_error"]=d;values["iq_recompute_error"]=q
    samples=list(map(int,re.findall(r"^>sample:(\d+)",text,re.M)))
    return dict(file=str(path),complete_samples=full,analysis_samples=len(times),
        metrics={k:stats(v) for k,v in values.items()},
        log_overrun=max(map(int,re.findall(r">log_overrun:(\d+)",text)),default=0),
        sample_steps=sorted(set(b-a for a,b in zip(samples,samples[1:]))),
        rpm=list(map(int,re.findall(r"rpm=(-?\d+)",text))),
        start=re.findall(r"FOC voltage start:.*",text),
        timing=re.findall(r"FOC timing:.*",text)[-1:])

if __name__=="__main__":
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--polarity",type=float,choices=[-1.0,1.0],default=1.0)
    parser.add_argument("--direction",type=float,choices=[-1.0,1.0],default=1.0)
    parser.add_argument("logs",nargs="+")
    args=parser.parse_args()
    result=[analyze(Path(p),args.polarity,args.direction) for p in args.logs]
    print(json.dumps(result,ensure_ascii=False,indent=2))
