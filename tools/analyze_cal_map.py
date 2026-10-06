"""Independently validate CALMAP CSV and recompute circular statistics."""
import argparse
import csv
import json
import math
from pathlib import Path
import re


def analyze(path):
    text = path.read_text(encoding='utf-8-sig')
    begin = next(line for line in text.splitlines() if line.startswith('CALMAP_BEGIN,'))
    config = dict(item.split('=', 1) for item in begin.split(',')[1:])
    points, pairs, direction = (int(config[key]) for key in ['points', 'pole_pairs', 'direction'])
    offset = float(config['offset'])
    rows = [row for row in csv.reader(text.splitlines()) if row and row[0] == 'CALMAP']
    assert 'CALMAP_END' in text, 'No successful completion marker'
    assert len(rows) == 2 * points, 'Unexpected total sample count'
    summary_text = text.split('CALMAP_SUMMARY', 1)[1].split('CALMAP_END', 1)[0]
    summary = dict(line.split('=', 1) for line in summary_text.splitlines() if '=' in line)
    result = {'log': str(path), 'config': config, 'firmware_summary': summary, 'passes': {}}
    errors_all = []
    means = []
    for label, name in [('F', 'forward'), ('R', 'reverse')]:
        samples = [row for row in rows if row[1] == label]
        assert [int(row[2]) for row in samples] == list(range(points)), 'Missing/duplicate index'
        assert all(len(row) == 13 for row in samples), 'Truncated CSV row'
        assert all(math.isfinite(float(value)) for row in samples for value in row[3:]), 'Nonfinite data'
        times = [int(row[3]) for row in samples]
        gaps = [b - a for a, b in zip(times, times[1:])]
        assert min(gaps) > 0, 'Non-increasing sample time'
        errors = [float(row[8]) for row in samples]
        mismatch = max(abs(math.remainder(direction * pairs * float(row[6]) - offset - float(row[5]) - float(row[8]), math.tau)) for row in samples)
        assert mismatch < 0.0001, 'Electrical error formula mismatch'
        assert all(abs(math.remainder(float(row[7]) - float(row[6]), math.tau)) < 0.0001 for row in samples), 'Unwrapped position mismatch'
        sin_sum, cos_sum = sum(map(math.sin, errors)), sum(map(math.cos, errors))
        mean = math.atan2(sin_sum, cos_sum)
        resultant = math.hypot(sin_sum, cos_sum) / points
        assert abs(mean - float(summary['mean_error_' + name])) < 0.0001
        assert abs(resultant - float(summary['resultant_' + name])) < 0.0001
        result['passes'][label] = {
            'count': len(samples), 'elapsed_first_ms': times[0], 'elapsed_last_ms': times[-1],
            'interval_min_ms': min(gaps), 'interval_max_ms': max(gaps),
            'mean_error_e_rad': mean, 'resultant': resultant,
            'error_min_e_rad': min(errors), 'error_max_e_rad': max(errors),
            'error_peak_to_peak_e_rad': max(errors) - min(errors),
            'max_logged_current_A': max(abs(float(value)) for row in samples for value in row[9:13]),
            'max_error_formula_mismatch_rad': mismatch,
            'csv_bytes_per_second': sum(len(','.join(row)) + 2 for row in samples) / (int(config['sweep_ms']) / 1000),
        }
        means.append(mean)
        errors_all.extend(errors)
    combined = math.atan2(sum(map(math.sin, errors_all)), sum(map(math.cos, errors_all)))
    candidate = (offset + combined) % math.tau
    difference = math.remainder(means[0] - means[1], math.tau)
    assert abs(candidate - float(summary['candidate_offset'])) < 0.0001
    assert abs(combined - float(summary['mean_error_combined'])) < 0.0001
    assert abs(difference - float(summary['bidirectional_difference'])) < 0.0001
    identities = re.findall(r'Calibration: VALID, stage=0, stored=(yes|no), direction=(-?1), offset=([0-9.]+)', text)
    assert len(identities) >= 2 and len(set(identities)) == 1, 'Calibration identity changed/missing'
    assert abs(float(summary['forward_travel']) - direction * math.tau) <= math.tau * 0.05
    assert abs(float(summary['return_error_e'])) <= 0.15
    result.update(mean_error_combined=combined, candidate_offset=candidate,
                  bidirectional_difference=difference, calibration_identity=identities[0],
                  serial_status=re.findall(r'Serial RX:[^\r\n]+', text),
                  encoder_status=re.findall(r'Encoder errors:[^\r\n]+', text))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', type=Path, nargs='+')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = json.dumps([analyze(path) for path in args.logs], ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(output + '\n', encoding='utf-8')
    print(output)
