#!/usr/bin/env python3
"""Collect matched 20M ROI TEA evidence for the final twelve-wide probe."""

import concurrent.futures
import configparser
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time


ROOT = Path(__file__).resolve().parents[4]
BASE = Path('/tmp/tea-upperbound-20261028')
OUTPUT = BASE / 'final-twelve-tea-roi-v2'
PROFILE = Path('/nfs/home/share/checkpoints_profiles/'
               'spec06_gcc16_rva23_novec_260820')
CASES = [('mcf', '6753'), ('perlbench_checkspam', '5886'),
         ('astar_rivers', '8610')]
KEYS = ['system.cpu.numCycles', 'system.cpu.committedInsts',
        'system.cpu.dcache.ReadReq.misses::total',
        'system.cpu.mmu.dtb.readMisses']
KEYS += ['system.cpu.branchPred.branchClassMisses::' + kind
         for kind in ['cond_branch', 'direct_call', 'indirect_call',
                      'return', 'direct_jump', 'indirect_jump', 'unknown']]


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    OUTPUT.mkdir(exist_ok=True)
    ref = Path(subprocess.check_output(
        ['python3', 'util/nemu_ref/resolve.py', 'normal-dedup'],
        cwd=ROOT, text=True).strip())
    binary = ROOT / 'build/RISCV/gem5.opt'
    fingerprint = {'binary_sha256': sha(binary), 'ref_sha256': sha(ref),
                   'ref': str(ref), 'source_head': subprocess.check_output(
                       ['git', 'rev-parse', 'HEAD'], cwd=ROOT,
                       text=True).strip(),
                   'performance_source':
                   '36584adaf67da1e848d7b3a61d5ab18e84b2c12e'}
    dispatch = json.loads((BASE / 'fourload-window-frontend-dispatch.json')
                          .read_text())
    candidate = shlex.split(dispatch['inputs']['extra_args'])
    archive_root = Path('/nfs/home/share/gem5_ci/performance_data/'
                        'spec06-rva23-novec-gcc16-0.3c')
    archives = {
        'baseline': archive_root / '20261028_135029_e34d8f6_kmhv3_run1667',
        'candidate': archive_root / '20261028_152834_36584adaf6_kmhv3_run1676'}
    spec = importlib.util.spec_from_file_location(
        'tea_summary', Path(__file__).with_name('summarize_tea.py'))
    parser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(parser)

    def run(workload, point, variant, tea):
        name = 'tea' if tea else 'off'
        dest = OUTPUT / f'{workload}_{point}' / variant / name
        dest.mkdir(parents=True, exist_ok=True)
        images = list((PROFILE / 'checkpoint' / workload / point)
                      .glob('*.zstd'))
        assert len(images) == 1, images
        command = [str(binary), f'--outdir={dest}',
                   'configs/example/kmhv3.py',
                   f'--generic-rv-cpt={images[0]}', '--enable-mem-dedup',
                   '--warmup-insts-no-switch=20000000',
                   '--maxinsts=40000000']
        if variant == 'candidate':
            command += candidate
        if tea:
            command += ['--enable-tea', '--tea-sample-period=1']
        meta = {**fingerprint, 'command': command,
                'checkpoint_sha256': sha(images[0]),
                'scope': '20M warmup + 20M ROI; representative point '
                         'diagnosis, not a weighted SPEC score'}
        manifest = dest / 'command.json'
        validation = dest / 'validation.json'
        if validation.exists():
            old = json.loads(manifest.read_text())
            assert old == meta, (dest, 'resume metadata changed')
            return json.loads(validation.read_text())
        assert not manifest.exists(), (dest, 'existing unfinished run')
        manifest.write_text(json.dumps(meta, indent=2) + '\n')
        started = time.time()
        with (dest / 'run.log').open('w') as log:
            proc = subprocess.run(
                command, cwd=ROOT, env={**os.environ, 'GCBV_REF_SO': str(ref)},
                stdout=log, stderr=subprocess.STDOUT, timeout=7200)
        log = (dest / 'run.log').read_text(errors='replace')
        assert proc.returncode == 0, (dest, proc.returncode, log[-600:])
        assert 'a thread reached the max instruction count' in log, dest
        assert 'Will trigger stat dump and reset' in log, dest
        assert not re.search(r'panic:|fatal:|Assertion .* failed|'
                             r'Difftest.*(?:failed|mismatch)', log, re.I)
        cfg = configparser.ConfigParser(interpolation=None, strict=False)
        cfg.optionxform = str
        cfg.read(dest / 'config.ini')
        cpu = cfg['system.cpu']
        assert cpu.getboolean('enable_difftest')
        assert cpu.getboolean('enableTea') == tea
        assert int(cpu['warmupInstCount']) == 20000000
        assert int(cpu['max_insts_any_thread']) == 40000000
        for key in ['decodeWidth', 'renameWidth', 'commitWidth',
                    'phyregReleaseWidth']:
            assert int(cpu[key]) == 12, (dest, key)
        assert int(cpu['numROBEntries']) == (640 if variant == 'candidate'
                                             else 352)
        stats_text = (dest / 'stats.txt').read_text()
        blocks = re.findall(r'Begin Simulation Statistics(.*?)'
                            r'End Simulation Statistics', stats_text, re.S)
        assert len(blocks) == 2, (dest, len(blocks))
        first_insts = float(re.search(r'^simInsts\s+(\d+)', blocks[0],
                                     re.M)[1])
        assert 19999968 <= first_insts <= 20000032
        values = parser.stats(dest / 'stats.txt')
        assert 19999968 <= values['system.cpu.committedInsts'] <= 20000032
        ci_stats = parser.stats(archives[variant] / 'spec_all' /
                                f'{workload}_{point}' / 'm5out/stats.txt')
        comparisons = {k: {'CI': ci_stats[k], 'local': values[k]}
                       for k in KEYS}
        assert all(x['CI'] == x['local'] for x in comparisons.values()), (
            dest, comparisons)
        record = {'workload': workload, 'point': point, 'variant': variant,
                  'tea': tea, 'elapsed_seconds': time.time() - started,
                  'cycles': values['system.cpu.numCycles'],
                  'instructions': values['system.cpu.committedInsts'],
                  'selected_stats': {k: values.get(k) for k in KEYS},
                  'config_sha256': sha(dest / 'config.ini'),
                  'stats_sha256': sha(dest / 'stats.txt'), 'passed': True,
                  'warmup_stats_instructions': first_insts,
                  'stats_blocks': len(blocks),
                  'ci_selected_counters_exact_match': comparisons}
        if tea:
            record['conservation'] = parser.point(dest)['conservation']
        validation.write_text(json.dumps(record, indent=2) + '\n')
        print(json.dumps(record), flush=True)
        return record

    jobs = [(w, p, v, True) for w, p in CASES
            for v in ['baseline', 'candidate']]
    jobs += [('mcf', '6753', v, False) for v in ['baseline', 'candidate']]
    jobs += [('perlbench_checkspam', '5886', 'candidate', False)]
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        futures = [pool.submit(run, *job) for job in jobs]
        for future in concurrent.futures.as_completed(futures):
            rows.append(future.result())
    observers = []
    for workload, point, variant, tea in jobs:
        if tea:
            continue
        dest = OUTPUT / f'{workload}_{point}' / variant
        a = parser.stats(dest / 'off' / 'stats.txt')
        b = parser.stats(dest / 'tea' / 'stats.txt')
        comparisons = {k: {'off': a.get(k), 'on': b.get(k)} for k in KEYS}
        assert all(x['off'] is not None and x['off'] == x['on']
                   for x in comparisons.values()), comparisons
        observers.append({'workload': workload, 'point': point,
                          'variant': variant, 'comparisons': comparisons,
                          'equivalence': 'selected counters exactly equal'})
    (OUTPUT / 'summary.json').write_text(json.dumps(
        {'runs': rows, 'observer_checks': observers}, indent=2) + '\n')


if __name__ == '__main__':
    main()
