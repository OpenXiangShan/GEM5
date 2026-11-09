#!/usr/bin/env python3
"""Join scalar Schedule events to retired O3PipeView fetch cohorts."""
import argparse
from collections import Counter, defaultdict
import gzip
import hashlib
import json
import math
from pathlib import Path
import re
import statistics

PREFIX = re.compile(r'^\s*(\d+):\s+([^:]+):\s+(.*)$')
SEQ = re.compile(r'\[sn[: ](\d+)\]')
EVENTS = {
    'insert': 'scheduler insert:',
    'ready': 'add to readyInstsQue',
    'selected': 'was selected',
    'scheduled': 'no conflict, scheduled',
    'retry': 'arbitration failed, retry',
    'to_fu': 'add to FUs',
}


def summarize(values):
    if not values:
        return {'count': 0}
    values = sorted(values)
    return {
        'count': len(values),
        'mean': statistics.fmean(values),
        'median': statistics.median(values),
        'p90': values[math.ceil(.9 * len(values)) - 1],
        'minimum': values[0],
        'maximum': values[-1],
    }


def analyze(trace, clock_ticks, fetch_start, fetch_end, pcs):
    if clock_ticks <= 0 or fetch_start >= fetch_end or not pcs:
        raise ValueError('positive clock, nonempty interval and PCs required')
    records, events, queues = [], defaultdict(lambda: defaultdict(list)), {}
    current = None
    with gzip.open(trace, 'rt') as stream:
        for line in stream:
            if line.startswith('O3PipeView:'):
                fields = line.rstrip('\n').split(':')
                stage, tick = fields[1], int(fields[2])
                if stage == 'fetch':
                    current = {'fetch': tick, 'pc': int(fields[3], 16),
                               'micro_pc': int(fields[4]), 'sn': int(fields[5])}
                elif current is not None:
                    current[stage] = tick
                    if stage == 'retire':
                        if (current['pc'] in pcs and tick > 0
                                and fetch_start <= current['fetch'] < fetch_end):
                            records.append(current)
                        current = None
                continue
            match = PREFIX.match(line)
            if not match:
                continue
            tick, message = int(match[1]), match[3]
            seq = SEQ.search(message)
            if not seq:
                continue
            sn = int(seq[1])
            for event, marker in EVENTS.items():
                if marker in message:
                    events[sn][event].append(tick)
            if 'insert into ' in message:
                queues[sn] = message.rsplit('insert into ', 1)[1].strip()
    by_sn = {r['sn']: r for r in records}
    if len(by_sn) != len(records) or any(r['micro_pc'] != 0 for r in records):
        raise ValueError('selected PCs require unique scalar sequence numbers')
    groups = {}
    for pc in sorted(pcs):
        group = [r for r in records if r['pc'] == pc]
        durations = defaultdict(list)
        counts, missing, iq = Counter(), Counter(), Counter()
        for record in group:
            sn, times = record['sn'], events[record['sn']]
            iq[queues.get(sn, 'unobserved')] += 1
            counts.update({name: len(values) for name, values in times.items()})
            stamps = {
                name: record[name]
                for name in ['fetch', 'decode', 'issue', 'complete', 'retire']
                if record.get(name, 0) > 0}
            for name in ['insert', 'ready', 'selected', 'scheduled', 'to_fu']:
                if times.get(name):
                    stamps['first_' + name] = min(times[name])
                    stamps['last_' + name] = max(times[name])
                else:
                    missing[name] += 1
            for first, second in [
                    ('fetch', 'first_insert'), ('decode', 'first_insert'),
                    ('first_insert', 'first_ready'),
                    ('first_ready', 'first_scheduled'),
                    ('first_insert', 'first_scheduled'), ('first_insert', 'issue'),
                    ('first_scheduled', 'last_scheduled'), ('last_scheduled', 'issue'),
                    ('issue', 'complete'), ('complete', 'retire')]:
                if first in stamps and second in stamps:
                    durations[first + '_to_' + second].append(
                        (stamps[second] - stamps[first]) / clock_ticks)
        groups[hex(pc)] = {
            'retired_instructions': len(group), 'IQ_distribution': dict(iq),
            'Schedule_log_calls': dict(counts),
            'missing_event_instructions': dict(missing),
            'timing_cycles': {
                name: summarize(values) for name, values in durations.items()},
        }
    pairs = defaultdict(lambda: defaultdict(list))
    for sn, producer in by_sn.items():
        consumer = by_sn.get(sn + 1)
        if consumer is None:
            continue
        key = hex(producer['pc']) + '->' + hex(consumer['pc'])
        for stage in ['first_scheduled', 'last_scheduled']:
            values = events[sn + 1].get('scheduled', [])
            if producer.get('complete', 0) > 0 and values:
                tick = min(values) if stage == 'first_scheduled' else max(values)
                pairs[key]['producer_complete_to_consumer_' + stage].append(
                    (tick - producer['complete']) / clock_ticks)
    with trace.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {
        'trace_sha256': digest, 'clock_ticks': clock_ticks,
        'fetch_start': fetch_start, 'fetch_end': fetch_end,
        'retired_selected_instructions': len(records), 'PCs': groups,
        'adjacent_sequence_pairs': {
            pair: {name: summarize(values) for name, values in measures.items()}
            for pair, measures in sorted(pairs.items())},
        'scope': (
            'Retired scalar instructions selected by fetch timestamps. '
            'Insert uses the actual scheduler insert log, not the zero '
            'O3PipeView dispatch field. Ready/scheduled events may repeat '
            'after cancellation; first/last are explicit. All timing '
            'offsets are signed; speculative wakeup may precede completion. '
            'Adjacent sequence pairs describe program order, not proof of '
            'data dependence. No whole-ROI, mutually exclusive stall '
            'attribution or recoverable-cycle claim.'),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--clock-ticks', type=int, required=True)
    parser.add_argument('--fetch-start', type=int, required=True)
    parser.add_argument('--fetch-end', type=int, required=True)
    parser.add_argument(
        '--pcs', nargs='+', type=lambda value: int(value, 0), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(
        args.trace, args.clock_ticks, args.fetch_start, args.fetch_end,
        set(args.pcs))
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()
