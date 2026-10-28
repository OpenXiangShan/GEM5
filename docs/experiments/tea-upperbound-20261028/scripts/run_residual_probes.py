#!/usr/bin/env python3
"""Run two single-axis probes selected from full-ROI counter evidence."""

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
REPORT = Path(__file__).resolve().parents[1]
OUTPUT = Path('/tmp/tea-upperbound-20261028/residual-single-axis')
PROBES = [
    ('mcf_6753', 'R5-fourload-window-frontend', 'mmu.dtb.size', '512'),
    ('astar_rivers_8610', 'R4-fourload-frontend', 'RAWQEntries', '128')]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def ini(path):
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.optionxform = str
    config.read(path)
    return {section + '.' + key: value for section in config.sections()
            for key, value in config.items(section)}


def config_delta(before, after):
    differences, equivalent_inputs = {}, {}
    for field in before.keys() | after.keys():
        a, b = before.get(field), after.get(field)
        if a == b:
            continue
        if field == 'system.mem_ctrls.configFile':
            assert a and b
            hashes = [digest(Path(path)) for path in [a, b]]
            assert hashes[0] == hashes[1], (field, hashes)
            equivalent_inputs[field] = {'baseline': a, 'probe': b,
                                        'sha256': hashes[0]}
        else:
            differences[field] = {'baseline': a, 'probe': b}
    return differences, equivalent_inputs


def main():
    OUTPUT.mkdir(exist_ok=True)
    runs = {row['label']: row for row in json.loads(
        (REPORT / 'runs.json').read_text())}
    spec = importlib.util.spec_from_file_location(
        'tea_summary', Path(__file__).with_name('summarize_tea.py'))
    parser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(parser)
    binary = ROOT / 'build/RISCV/gem5.opt'
    ref = Path(subprocess.check_output(
        ['python3', 'util/nemu_ref/resolve.py', 'normal-dedup'],
        cwd=ROOT, text=True).strip())
    identity = {'binary_sha256': digest(binary), 'ref_sha256': digest(ref),
                'ref': str(ref), 'performance_source':
                '36584adaf67da1e848d7b3a61d5ab18e84b2c12e'}

    def run(case, label, key, value):
        baseline = runs[label]
        point = Path(baseline['archive_path']) / 'spec_all' / case
        workload, number = case.rsplit('_', 1)
        profile = Path('/nfs/home/share/checkpoints_profiles/'
                       'spec06_gcc16_rva23_novec_260820/checkpoint')
        images = list((profile / workload / number).glob('*.zstd'))
        assert len(images) == 1
        dest = OUTPUT / (case + '-' + key.replace('.', '-') + '-' + value)
        dest.mkdir(exist_ok=True)
        target = 'system.cpu.' + key
        arguments = shlex.split(baseline['dispatch']['inputs']['extra_args'])
        prefix = '--param=system.cpu[0].' + key + '='
        arguments = [a for a in arguments if not a.startswith(prefix)]
        arguments.append(prefix + value)
        command = [str(binary), f'--outdir={dest}',
                   'configs/example/kmhv3.py',
                   f'--generic-rv-cpt={images[0]}', '--enable-mem-dedup',
                   '--warmup-insts-no-switch=20000000',
                   '--maxinsts=40000000', *arguments]
        metadata = {**identity, 'command': command,
                    'checkpoint_sha256': digest(images[0]),
                    'baseline_ci': str(point), 'changed_parameter': target,
                    'scope': '20M warmup + 20M ROI; one point; '
                             'not a weighted SPEC score'}
        manifest = dest / 'command.json'
        assert not manifest.exists(), 'Do not silently restart a probe'
        manifest.write_text(json.dumps(metadata, indent=2) + '\n')
        start = time.time()
        with (dest / 'run.log').open('w') as log:
            process = subprocess.run(
                command, cwd=ROOT, env={**os.environ, 'GCBV_REF_SO': str(ref)},
                stdout=log, stderr=subprocess.STDOUT, timeout=7200)
        log = (dest / 'run.log').read_text(errors='replace')
        assert process.returncode == 0
        assert 'a thread reached the max instruction count' in log
        assert 'Will trigger stat dump and reset' in log
        assert not re.search(r'panic:|fatal:|Assertion .* failed|'
                             r'Difftest.*(?:failed|mismatch)', log, re.I)
        before, after = ini(point / 'm5out/config.ini'), ini(dest / 'config.ini')
        differences, paths = config_delta(before, after)
        assert set(differences) == {target}, differences
        assert after[target] == value
        assert after['system.cpu.enable_difftest'] == 'true'
        assert after['system.cpu.warmupInstCount'] == '20000000'
        assert after['system.cpu.max_insts_any_thread'] == '40000000'
        for width in ['decodeWidth', 'renameWidth', 'commitWidth',
                      'phyregReleaseWidth']:
            assert after['system.cpu.' + width] == '12'
        blocks = re.findall(r'Begin Simulation Statistics(.*?)'
                            r'End Simulation Statistics',
                            (dest / 'stats.txt').read_text(), re.S)
        assert len(blocks) == 2
        for block in blocks:
            assert 19999968 <= float(re.search(
                r'^simInsts\s+(\d+)', block, re.M)[1]) <= 20000032
        a, b = parser.stats(point / 'm5out/stats.txt'), parser.stats(
            dest / 'stats.txt')
        cpi = [s['system.cpu.numCycles'] / s['system.cpu.committedInsts']
               for s in [a, b]]
        record = {'case': case, 'parameter': target, 'value': value,
                  'baseline_label': label, 'config_changes': differences,
                  'equivalent_input_paths': paths, 'cpi': cpi,
                  'ipc_gain_pct': (cpi[0] / cpi[1] - 1) * 100,
                  'elapsed_seconds': time.time() - start,
                  'stats_sha256': digest(dest / 'stats.txt'), 'passed': True}
        (dest / 'validation.json').write_text(
            json.dumps(record, indent=2) + '\n')
        print(json.dumps(record), flush=True)
        return record

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        rows = list(pool.map(lambda probe: run(*probe), PROBES))
    (OUTPUT / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    main()
