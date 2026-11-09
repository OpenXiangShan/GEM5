#!/usr/bin/env python3
"""Join observed physical-register dependencies to scalar Schedule issue events."""
import argparse
from collections import Counter, defaultdict
import gzip
import hashlib
import json
from pathlib import Path
import re

from analyze_schedule_cohort import PREFIX, SEQ, summarize

DEST = re.compile(r'mark scoreboard p(\d+) not ready')
SOURCE = re.compile(r'\[sn:(\d+)\] src p(\d+) add to depGraph')


def analyze(trace, clock_ticks, fetch_start, fetch_end, pcs):
    if clock_ticks <= 0 or fetch_start >= fetch_end or not pcs:
        raise ValueError('positive clock, nonempty interval and PCs required')
    owners, dependencies, calls = {}, defaultdict(set), Counter()
    pipeline, issues, ready, disassembly = {}, defaultdict(list), {}, {}
    producer_context, current = None, None
    selected_seen, ambiguous = set(), set()
    with gzip.open(trace, 'rt') as stream:
        for line in stream:
            if line.startswith('O3PipeView:'):
                fields = line.rstrip('\n').split(':')
                stage, tick = fields[1], int(fields[2])
                if stage == 'fetch':
                    current = dict(fetch=tick, pc=int(fields[3], 16),
                                   micro_pc=int(fields[4]), sn=int(fields[5]))
                elif current is not None:
                    current[stage] = tick
                    if stage == 'retire':
                        sn = current['sn']
                        if (current['pc'] in pcs and tick > 0
                                and fetch_start <= current['fetch'] < fetch_end):
                            if sn in selected_seen:
                                raise ValueError('ambiguous selected sequence')
                            selected_seen.add(sn)
                        if sn in pipeline:
                            ambiguous.add(sn)
                        pipeline[sn] = (
                            current if sn not in ambiguous else None)
                        current = None
                producer_context = None
                continue
            match = PREFIX.match(line)
            if not match:
                producer_context = None
                continue
            tick, message = int(match[1]), match[3]
            seq = SEQ.search(message)
            if 'addProdecer' in message and seq:
                producer_context = int(seq[1])
                continue
            destination = DEST.fullmatch(message)
            if destination:
                if producer_context is None:
                    raise ValueError('destination log lacks producer context')
                owners[int(destination[1])] = producer_context
                continue
            producer_context = None
            source = SOURCE.search(message)
            if source:
                sn, reg = int(source[1]), int(source[2])
                calls[sn] += 1
                dependencies[sn].add((reg, owners.get(reg)))
            if seq:
                sn = int(seq[1])
                if 'add to FUs' in message:
                    issues[sn].append(tick)
                if 'add to readyInstsQue' in message:
                    ready[sn] = min(tick, ready.get(sn, tick))
                if 'scheduler insert:' in message:
                    disassembly[sn] = message.split('scheduler insert:', 1)[1].strip()
    selected = [row for row in pipeline.values() if row is not None
                and row['pc'] in pcs and row['retire'] > 0
                and fetch_start <= row['fetch'] < fetch_end]
    if any(row['micro_pc'] != 0 for row in selected):
        raise ValueError('selected PCs require scalar instructions')
    groups = {}
    for pc in sorted(pcs):
        group = [row for row in selected if row['pc'] == pc]
        owner_pcs, names, gaps = Counter(), Counter(), defaultdict(list)
        missing = Counter()
        observed_consumer_issues = 0
        for consumer in group:
            sn = consumer['sn']
            if issues[sn] and consumer.get('issue', 0) > 0:
                if max(issues[sn]) != consumer['issue']:
                    raise ValueError('Schedule/O3 issue timestamps disagree')
                observed_consumer_issues += 1
            elif consumer.get('issue', 0) > 0:
                missing['consumer_to_fu_with_O3_issue'] += 1
            for reg, producer in dependencies[sn]:
                previous = pipeline.get(producer)
                label = hex(previous['pc']) if previous else 'unobserved'
                owner_pcs[label] += 1
                names[disassembly.get(producer, 'unobserved')] += 1
                if producer is None:
                    missing['producer_before_trace_or_unobserved'] += 1
                if producer in ambiguous:
                    missing['ambiguous_producer_sequence'] += 1
                    continue
                if not issues[producer]:
                    missing['producer_to_fu'] += 1
                    continue
                endpoints = {}
                if issues[sn]:
                    endpoints['consumer_last_to_fu'] = max(issues[sn])
                else:
                    missing['consumer_to_fu'] += 1
                if sn in ready:
                    endpoints['consumer_first_ready'] = ready[sn]
                else:
                    missing['consumer_ready'] += 1
                for name, value in endpoints.items():
                    gaps['producer_last_to_fu_to_' + name].append(
                        (value - max(issues[producer])) / clock_ticks)
        groups[hex(pc)] = dict(
            retired_consumer_instructions=len(group),
            observed_consumer_issue_times=observed_consumer_issues,
            dependency_log_calls=sum(calls[row['sn']] for row in group),
            distinct_registered_physical_dependencies=sum(
                len(dependencies[row['sn']]) for row in group),
            producer_pipeline_PC_distribution=dict(owner_pcs),
            producer_scheduled_instruction_distribution=dict(names),
            missing_observations=dict(missing),
            edge_timing_cycles={name: summarize(values)
                                for name, values in gaps.items()})
    with trace.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return dict(
        trace_sha256=digest, clock_ticks=clock_ticks, fetch_start=fetch_start,
        fetch_end=fetch_end, retired_selected_instructions=len(selected), PCs=groups,
        scope=('Producer ownership is captured at each dependency registration, '
               'before physical-register reuse can change it. Destinations use '
               'the immediately preceding addProdecer log context. Dependencies '
               'are distinct consumer/register/producer observations, not all '
               'source operands, RF reads or saved cycles. Pipeline PC may name '
               'an original fusion record; actual scheduled disassembly is '
               'reported separately. Issue times use observed add-to-FUs logs, '
               'including producers without retired pipeline records. Gaps are '
               'signed per edge and may include speculative retries. Unknown '
               'producers/timestamps remain unknown; no exclusive stall or '
               'whole-ROI causal claim.'))


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
    result = analyze(args.trace, args.clock_ticks, args.fetch_start,
                     args.fetch_end, set(args.pcs))
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()
