#!/usr/bin/env python3
"""Compare two single-thread, period-one TEA measurement windows."""

import argparse
from collections import Counter, defaultdict
import configparser
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess


STATES = ['Computing', 'Stalled', 'Flushed', 'Drained']
EVENTS = ['DR_L1', 'DR_TLB', 'DR_SQ', 'FL_MB', 'FL_EX', 'FL_MO',
          'ST_L1', 'ST_TLB', 'ST_LLC']


def near(actual, expected):
    assert math.isclose(actual, expected, rel_tol=1e-8, abs_tol=1e-3), (
        actual, expected)


def stats(path):
    blocks = re.findall(r'Begin Simulation Statistics(.*?)'
                        r'End Simulation Statistics', path.read_text(), re.S)
    assert blocks, path
    result = {}
    for line in blocks[-1].splitlines():
        fields = line.split()
        if len(fields) > 1:
            try:
                result[fields[0]] = float(fields[1])
            except ValueError:
                pass
    return result


def point(directory):
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.read(directory / 'config.ini')
    assert config.getboolean('system.cpu', 'enable_difftest')
    assert config.getboolean('system.cpu', 'enableTea')
    assert config.getint('system.cpu', 'teaSamplePeriod') == 1
    log = (directory / 'run.log').read_text()
    assert 'a thread reached the max instruction count' in log
    assert not re.search(r'panic:|fatal:|Difftest.*(?:failed|mismatch)',
                         log, re.I)
    values = stats(directory / 'stats.txt')
    blocks = re.findall(r'Begin Simulation Statistics(.*?)'
                        r'End Simulation Statistics',
                        (directory / 'stats.txt').read_text(), re.S)
    warmup = config.getint('system.cpu', 'warmupInstCount')
    maximum = config.getint('system.cpu', 'max_insts_any_thread')
    reset = ('Will trigger stat dump and reset' in log and len(blocks) >= 2)
    window = {'configured_warmup': warmup, 'configured_total_limit': maximum,
              'complete_stats_blocks': len(blocks),
              'warmup_completed_before_measurement': reset,
              'kind': 'post-warmup ROI' if reset else 'restore prefix',
              'executed_warmup': warmup if reset else 0}
    insts = values['system.cpu.committedInsts']
    tea = {k.removeprefix('system.cpu.tea.'): v for k, v in values.items()
           if k.startswith('system.cpu.tea.')}
    near(tea['samples'], values['system.cpu.numCycles'])
    near(tea['committedByThread'], insts)
    near(sum(tea['samplesByState::' + s] for s in STATES), tea['samples'])
    totals, events = Counter(), Counter()
    pcs = defaultdict(Counter)
    with (directory / 'system.cpu.tea.csv').open() as stream:
        for row in csv.DictReader(stream):
            psv, weight = int(row['psv']), float(row['sample_weight'])
            assert row['thread'] == '0'
            assert math.isfinite(weight) and weight >= 0
            totals['all'] += weight
            totals['attributed' if psv >= 0 else str(psv)] += weight
            if psv < 0:
                assert psv in [-1, -2, -3]
                continue
            assert psv < (1 << len(EVENTS))
            pc = hex(int(row['pc'], 16))
            pcs[pc]['total'] += weight / insts
            for bit, event in enumerate(EVENTS):
                if psv & (1 << bit):
                    events[event] += weight
                    pcs[pc][event] += weight / insts
    for key, value in {
            'all': tea['samples'], 'attributed': tea['attributedSamples'],
            '-1': tea['cancelledTargets'], '-2': tea['pendingSamples'],
            '-3': tea['unresolvedTargets']}.items():
        near(totals[key], value)
    for event in EVENTS:
        near(events[event], tea['samplesByEvent::' + event])
    return {
        'directory': str(directory), 'instructions': insts, 'window': window,
        'cycles': values['system.cpu.numCycles'],
        'ipc': insts / values['system.cpu.numCycles'],
        'state_cpi': {s: tea['samplesByState::' + s] / insts for s in STATES},
        'event_cpi': {e: tea['samplesByEvent::' + e] / insts for e in EVENTS},
        'unattributed_cpi': {
            k: tea[k] / insts for k in
            ['cancelledTargets', 'pendingSamples', 'unresolvedTargets']},
        'pc_event_cpi': dict(pcs), 'conservation': 'passed',
        'stats_sha256': hashlib.sha256(
            (directory / 'stats.txt').read_bytes()).hexdigest(),
        'tea_csv_sha256': hashlib.sha256(
            (directory / 'system.cpu.tea.csv').read_bytes()).hexdigest()}


def disassemble(path):
    text = subprocess.check_output([
        'riscv64-linux-gnu-objdump', '-d', '--no-show-raw-insn',
        '--demangle', str(path)], text=True)
    function, result = '', {}
    for line in text.splitlines():
        header = re.match(r'^([0-9a-f]+) <(.*)>:$', line)
        inst = re.match(r'^\s*([0-9a-f]+):\s+(.+)$', line)
        if header:
            function = header[2]
        elif inst:
            result[hex(int(inst[1], 16))] = (function, inst[2].strip())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--name', required=True)
    parser.add_argument('--elf', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sides = [point(p) for p in [args.baseline, args.candidate]]
    symbols = disassemble(args.elf) if args.elf else {}
    all_pcs = set(sides[0]['pc_event_cpi']) | set(sides[1]['pc_event_cpi'])
    ranked = []
    for pc in all_pcs:
        cells = [s['pc_event_cpi'].get(pc, {}) for s in sides]
        left, right = [v.get('total', 0) for v in cells]
        ranked.append({
            'pc': pc, 'baseline_cpi': left, 'candidate_cpi': right,
            'delta_cpi': right - left,
            'function': symbols.get(pc, ('unmapped', ''))[0],
            'instruction': symbols.get(pc, ('', ''))[1],
            'events': [{e: v.get(e, 0) for e in EVENTS} for v in cells]})
    ranked.sort(key=lambda v: abs(v['delta_cpi']), reverse=True)
    top_baseline = sorted(ranked, key=lambda v: v['baseline_cpi'],
                          reverse=True)[:5]
    top_candidate = sorted(ranked, key=lambda v: v['candidate_cpi'],
                           reverse=True)[:5]
    event_tops = []
    for side in range(2):
        events = {}
        for event in EVENTS:
            rows = [row for row in ranked if row['events'][side][event] > 0]
            rows.sort(key=lambda row: row['events'][side][event], reverse=True)
            events[event] = [
                {'pc': row['pc'], 'function': row['function'],
                 'instruction': row['instruction'],
                 'event_cpi': row['events'][side][event]}
                for row in rows[:5]]
        event_tops.append(events)
    assert sides[0]['window'] == sides[1]['window'], 'Unmatched windows'
    window = sides[0]['window']
    scope = (f"{window['executed_warmup']} executed warmup instructions; "
             f"{window['kind']}; approximately "
             f"{sides[0]['instructions']:.0f} measured instructions; "
             'not a weighted SPEC score; PC weights include Computing '
             'and overlapping events')
    result = {'name': args.name, 'sides': sides, 'ranked_pcs': ranked,
              'top_baseline_pcs': top_baseline,
              'top_candidate_pcs': top_candidate,
              'event_top5_by_side': event_tops,
              'scope': scope}
    if args.elf:
        result['elf'] = str(args.elf)
        result['elf_sha256'] = hashlib.sha256(args.elf.read_bytes()).hexdigest()
    prefix = args.output / args.name
    prefix.with_suffix('.json').write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    lines = [f'# {args.name} TEA残余诊断', '',
             scope + '。状态互斥，事件可重叠。'
             'PC总权重包含Computing，不能视为可移除停顿。', '',
             '| 状态CPI | Baseline | Combined | ΔCPI |',
             '| --- | ---: | ---: | ---: |']
    for state in STATES:
        a, b = [s['state_cpi'][state] for s in sides]
        lines.append(f'| {state} | {a:.6f} | {b:.6f} | {b-a:+.6f} |')
    lines += ['', '| 事件归因CPI | Baseline | Combined | ΔCPI |',
              '| --- | ---: | ---: | ---: |']
    for event in EVENTS:
        a, b = [s['event_cpi'][event] for s in sides]
        lines.append(f'| {event} | {a:.6f} | {b:.6f} | {b-a:+.6f} |')
    lines += ['', '## 当前组合Top 5', '',
              '按当前组合的PC总归因排序；占比包含Computing，'
              '不代表可优化空间。事件热点另列于JSON，不以Top 1决定方向。', '',
              '| PC | 函数与指令 | Baseline总归因CPI | '
              'Combined总归因CPI | ΔCPI |',
              '| --- | --- | ---: | ---: | ---: |']
    for row in top_candidate:
        label = (row['function'] + ' ' + row['instruction']).replace('|', '/')
        lines.append(f"| {row['pc']} | {label} | "
                     f"{row['baseline_cpi']:.6f} | "
                     f"{row['candidate_cpi']:.6f} | "
                     f"{row['delta_cpi']:+.6f} |")
    lines += ['', '基线Top 5：' + '、'.join(
        row['pc'] for row in top_baseline) + '。各事件只列非零热点；'
        '原始PC差异排名保留在JSON，差额不是单个组件的净收益。']
    lines += ['', '四状态与CSV/PSV权重守恒检查通过。CSV没有PC×四状态联合字段；'
              '本表只对PC展示总权重，事件明细保留在JSON。守恒不证明观察等价，'
              '关闭TEA对照以单独验收记录为准；不得将PC差额视为单组件因果收益。']
    prefix.with_suffix('.md').write_text('\n'.join(lines) + '\n')
    print(json.dumps({'name': args.name, 'ipc': [s['ipc'] for s in sides],
                      'checks': 'passed'}, ensure_ascii=False))


if __name__ == '__main__':
    main()
