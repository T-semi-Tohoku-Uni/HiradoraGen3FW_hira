"""FOCの200 Hz時刻差ログを照合する。生ログを書き換えずに集計する。"""
import json,re,statistics,sys
from pathlib import Path

def analyze(path):
    text=path.read_text(encoding="utf-8")
    keys=["u1_a","v_a","u2_a","w_a","sector","id_a","iq_a","elec_rad","angle_age_us","angle_rx_age_us"]
    rows={key:re.findall(r"^>"+key+r":([\d.]+):(-?[\d.]+)$",text,re.M) for key in keys}
    baseline=[t for t,v in rows["u1_a"]]
    assert baseline, "No samples"
    for key in keys:
        assert [t for t,v in rows[key]]==baseline, f"Timestamp mismatch: {key}"
    samples=list(map(int,re.findall(r"^>sample:(\d+)$",text,re.M)))
    assert len(samples)==len(baseline)
    assert all((b-a)&0xffffffff==100 for a,b in zip(samples,samples[1:])), "Sample loss"
    overruns=list(map(int,re.findall(r"^>log_overrun:(\d+)$",text,re.M)))
    assert len(overruns)==len(samples) and set(overruns)=={0}, "Logger overrun"
    request=[int(v) for t,v in rows["angle_age_us"]]
    received=[int(v) for t,v in rows["angle_rx_age_us"]]
    assert all(0<=r<=q<=250 for q,r in zip(request,received)), "Invalid timing order/range"
    def stats(a):
        a=sorted(a)
        return dict(min=a[0],median=statistics.median(a),p95=a[int(.95*(len(a)-1))],max=a[-1])
    span=float(baseline[-1])-float(baseline[0])
    return dict(file=str(path),samples=len(samples),span_ms=span,hz=(len(samples)-1)*1000/span,
        request_age_us=stats(request),received_age_us=stats(received),
        timing=re.findall(r"^FOC timing:.*",text,re.M)[-1:],
        serial=re.findall(r"^Serial RX:.*",text,re.M)[-1:],
        encoder=re.findall(r"^Encoder errors:.*",text,re.M)[-1:])

if __name__=="__main__":
    print(json.dumps([analyze(Path(p)) for p in sys.argv[1:]],indent=2))
