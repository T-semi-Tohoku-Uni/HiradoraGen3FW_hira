"""Analyze stopped-only angle timing dump records; input timestamps are DWT cycles."""
import json
import re
import sys
from pathlib import Path


def analyze(path):
    text = Path(path).read_text(encoding='utf-8-sig')
    blocks = re.findall(r'TIMING status armed=0 pending=0 ready=1 hz=(\d+)\s+'
                        r'TIMING transfer ([\d,]+)\s+TIMING wait ([\d,]+)\s+'
                        r'TIMING next ([\d,]+)', text)
    result = []
    for i, (hz, transfer, wait, next_use) in enumerate(blocks):
        req, irq1, irq2, decoding, received, publish, address = map(int, transfer.split(','))
        adc, use, seq, used_req, skipped, ipsr, dma_active, adc_active = map(int, wait.split(','))
        new_seq, next_time, next_seq, next_req = map(int, next_use.split(','))
        def us(end, start=req):
            return ((end-start) & 0xffffffff)*1e6/int(hz)
        assert ipsr == 34 and dma_active == adc_active == 1
        assert address == 0x3fff and next_req == req
        assert next_seq == new_seq == (seq+1) & 0xffffffff
        assert us(decoding) < us(adc) < us(use) < us(skipped) < us(publish) < us(next_time)
        result.append(dict(capture=i, request_irq_us=us(irq1) if irq1 else None,
            response_irq_us=us(irq2), decoding_state_us=us(decoding),
            adc_begin_us=us(adc), old_sample_use_us=us(use), request_skipped_us=us(skipped),
            received_marker_us=us(received), publish_us=us(publish), next_sample_use_us=us(next_time),
            old_sequence=seq, new_sequence=new_seq, old_sample_age_us=us(use,used_req),
            next_sample_age_us=us(next_time,next_req), adc_use_gap_us=us(next_time,use),
            wait_to_publish_us=us(publish,skipped), dma_and_adc_active=True))
    return result


if __name__ == '__main__':
    print(json.dumps({p: analyze(p) for p in sys.argv[1:]}, indent=2))
