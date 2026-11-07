#!/usr/bin/env python3
"""Summarize audited paired or four-corner points against a weighted baseline."""
import argparse
import hashlib
import json
import math
from pathlib import Path


def keyed_rows(rows):
    result = {row['point']: row for row in rows}
    if len(result) != len(rows):
        raise ValueError('duplicate measured point')
    return result


def cpi(row):
    cycles, instructions = row['cycles'], row['instructions']
    if not all(math.isfinite(v) and v > 0 for v in [cycles, instructions]):
        raise ValueError('cycles and retired instructions must be positive')
    return cycles / instructions


def signs(values):
    return {
        'positive': sum(v > 0 for v in values),
        'negative': sum(v < 0 for v in values),
        'zero': sum(v == 0 for v in values),
    }


def summarize(profile, baseline, audit, corners):
    if len(corners) not in [2, 4] or len(set(corners)) != len(corners):
        raise ValueError('two or four distinct corner names are required')
    control = corners[0]
    candidates = corners[1:]
    if len(corners) == 4:
        _, factor_a, factor_b, combined = corners
    if not baseline['passed'] or not audit['passed'] or audit['failures']:
        raise ValueError('input audits must pass without failed points')
    full = keyed_rows(baseline['rows'])
    measured = keyed_rows(audit['rows'])
    pending = set(audit['pending'])
    if (len(measured) != audit['completed_points']
            or len(pending) != len(audit['pending'])
            or pending & measured.keys()
            or len(measured) + len(pending) != audit['expected_points']
            or audit['complete'] != (not pending)):
        raise ValueError('measured and pending cohort coverage is inconsistent')
    scales, baseline_time = {}, {}
    for workload, data in profile.items():
        benchmark = workload.split('_')[0]
        weights = {p: float(w) for p, w in data['points'].items()}
        if not weights or not all(math.isfinite(w) and w >= 0
                                  for w in weights.values()):
            raise ValueError('profile weights must be finite and nonnegative')
        mass = math.fsum(weights.values())
        if mass <= 0 or int(data['insts']) <= 0:
            raise ValueError('input instructions and weight sum must be positive')
        for point, weight in weights.items():
            name = workload + '_' + point
            scale = int(data['insts']) * weight / mass
            scales[name] = (benchmark, scale)
    if full.keys() != scales.keys() or baseline['points'] != len(scales):
        raise ValueError('baseline must cover the exact complete profile')
    if not (measured.keys() | pending) <= scales.keys():
        raise ValueError('experiment point is absent from the full profile')
    for point, (benchmark, scale) in scales.items():
        baseline_time[benchmark] = baseline_time.get(benchmark, 0) + scale * cpi(full[point])
    rows, benchmarks = [], {}
    for point, row in measured.items():
        metrics = row['measured']
        original = full[point]
        before = metrics[control]
        if any(before[k] != original[k] for k in ['cycles', 'instructions']):
            raise ValueError('control window differs from full baseline: ' + point)
        values = {name: cpi(metrics[name]) for name in corners}
        benchmark, scale = scales[point]
        normalization = 100 * scale / baseline_time[benchmark]
        deltas = {name: normalization * (values[name] - values[control])
                  for name in candidates}
        marginal = ({name: normalization * (values[combined] - values[name])
                     for name in [factor_a, factor_b]}
                    if len(corners) == 4 else {})
        share = scale * values[control] / baseline_time[benchmark]
        rows.append({
            'point': point, 'benchmark': benchmark,
            'full_baseline_benchmark_time_share': share,
            'speedup_pct': {name: 100 * (values[control] / values[name] - 1)
                            for name in candidates},
            'measured_time_delta_pct_of_full_baseline': deltas,
        })
        if len(corners) == 4:
            rows[-1].update({
                'combined_marginal_speedup_pct': {
                    name: 100 * (values[name] / values[combined] - 1)
                    for name in [factor_a, factor_b]},
                'combined_marginal_time_delta_pct_of_full_baseline': marginal,
                'interaction_time_delta_pct_of_full_baseline': normalization * (
                    values[combined] - values[factor_a] - values[factor_b]
                    + values[control]),
            })
        totals = benchmarks.setdefault(benchmark, {
            'measured_points': 0, 'measured_baseline_time_share': 0,
            'measured_time_delta_pct_of_full_baseline': dict.fromkeys(deltas, 0),
        })
        if marginal:
            totals.setdefault('combined_marginal_time_delta_pct_of_full_baseline',
                              dict.fromkeys(marginal, 0))
        totals['measured_points'] += 1
        totals['measured_baseline_time_share'] += share
        for name in deltas:
            totals['measured_time_delta_pct_of_full_baseline'][name] += deltas[name]
        for name in marginal:
            totals['combined_marginal_time_delta_pct_of_full_baseline'][name] += marginal[name]
    result = {
        'profile_points': len(scales), 'measured_points': len(measured),
        'unknown_profile_points': len(scales) - len(measured),
        'pending_cohort_points': audit['pending'], 'cohort_complete': audit['complete'],
        'corners': dict(zip(['control', 'candidate'] if len(corners) == 2
                            else ['control', 'factor_a', 'factor_b', 'combined'],
                            corners)),
        'sign_counts': {name: signs([r['speedup_pct'][name] for r in rows])
                        for name in candidates},
        'rows': rows, 'benchmarks': benchmarks,
        'scope': ('Only measured point contributions normalized to complete '
                  'original baseline benchmark time. Unknown candidate points '
                  'remain unknown. Time deltas are percentage points of '
                  'baseline time; negative is beneficial. No full benchmark '
                  'gain, suite score, resource or correctness re-audit.'),
    }
    if len(corners) == 4:
        result['combined_marginal_sign_counts'] = {
            name: signs([r['combined_marginal_speedup_pct'][name] for r in rows])
            for name in [factor_a, factor_b]}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--baseline-audit', type=Path, required=True)
    parser.add_argument('--experiment-audit', type=Path, required=True)
    parser.add_argument('--corners', nargs='+', required=True, metavar='CORNER',
                        help='CONTROL CANDIDATE, or CONTROL A B COMBINED')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    inputs = [args.profile, args.baseline_audit, args.experiment_audit]
    payloads = [p.read_bytes() for p in inputs]
    try:
        result = summarize(*(json.loads(b) for b in payloads), args.corners)
    except ValueError as error:
        parser.error(str(error))
    result['source_sha256'] = {str(p): hashlib.sha256(b).hexdigest()
                               for p, b in zip(inputs, payloads)}
    with args.output.open('x') as stream:
        stream.write(json.dumps(result, indent=2, allow_nan=False) + '\n')
    sign_key = ('combined_marginal_sign_counts' if len(args.corners) == 4
                else 'sign_counts')
    print(json.dumps({k: result[k] for k in [
        'measured_points', 'unknown_profile_points', sign_key]}, indent=2))


if __name__ == '__main__':
    main()
