#!/usr/bin/env python3
# Copyright (c) 2026 XiangShan
# SPDX-License-Identifier: BSD-3-Clause
"""Compare fixed checkpoint sets containing only successful complete points."""

import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def metadata(root):
    values = {}
    for line in (root / 'metadata.txt').read_text().splitlines():
        key, separator, value = line.partition(':')
        if separator:
            values[key.strip()] = value.strip()
    return values


def configuration(root, point):
    config = configparser.ConfigParser(interpolation=None)
    config.read(root / 'spec_all' / point / 'm5out/config.ini')
    cpu = config['system.cpu']
    prefetcher = {
        section: dict(config[section]) for section in config.sections()
        if section.startswith('system.cpu.dcache.prefetcher')
    }
    return {
        'max_insts': cpu['max_insts_any_thread'],
        'warmup_insts': cpu['warmupinstcount'],
        'prefetcher': prefetcher,
        'clock': dict(config[cpu['clk_domain']]),
        'l2_geometry': {
            section: {key: config[section][key]
                      for key in ['size', 'assoc', 'tag_latency',
                                  'data_latency', 'response_latency',
                                  'mshrs', 'tgts_per_mshr', 'write_buffers']
                      if key in config[section]}
            for section in config.sections()
            if section.startswith('system.l2') and
            config.has_section(section + '.replacement_policy')
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--weights', type=Path, required=True)
    parser.add_argument('--run', action='append', required=True,
                        metavar='LABEL=ARCHIVE')
    parser.add_argument('--out-dir', type=Path, required=True)
    parser.add_argument('--processor', type=Path,
                        default=Path(os.environ.get(
                            'GEM5_DATA_PROC_HOME',
                            '/nfs/home/share/gem5_ci/gem5_data_proc')))
    args = parser.parse_args()
    weights = args.weights.resolve()
    clusters = json.loads(weights.read_text())
    points = sorted(f'{workload}_{point}'
                    for workload, data in clusters.items()
                    for point in data['points'])
    if not points:
        raise SystemExit('No checkpoint points in the weights JSON')
    if any(not re.fullmatch(r'[A-Za-z0-9_.-]+_[0-9]+', p) for p in points):
        raise SystemExit('Checkpoint identifiers must be simple path names')
    if not (args.processor / 'run.py').is_file():
        raise SystemExit('Set --processor to an existing gem5_data_proc tree')
    runs = {}
    for value in args.run:
        label, separator, archive = value.partition('=')
        if (not separator or not re.fullmatch(r'[A-Za-z0-9_-]+', label)
                or label in runs):
            raise SystemExit('Use unique LABEL=ARCHIVE values')
        root = Path(archive).resolve()
        meta = metadata(root)
        failed = []
        for point in points:
            directory = root / 'spec_all' / point
            stats = directory / 'm5out/stats.txt'
            if (not (directory / 'completed').is_file()
                    or (directory / 'abort').exists()
                    or not stats.is_file() or stats.stat().st_size == 0):
                failed.append(point)
        if failed:
            detail = ', '.join(failed[:12])
            raise SystemExit(f'{label}: {len(failed)} incomplete/failed '
                             f'points: {detail}')
        runs[label] = {'root': root, 'metadata': meta,
                       'configuration': configuration(root, points[0])}
    reference = next(iter(runs.values()))
    for label, run in runs.items():
        for key in ['config_path', 'checkpoint_root', 'ref_so']:
            if run['metadata'].get(key) != reference['metadata'].get(key):
                raise SystemExit(f'{label}: incompatible {key}')
        if run['configuration'] != reference['configuration']:
            raise SystemExit(f'{label}: incompatible configuration')
    output = args.out_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    manifest = {
        'points': len(points),
        'weights': str(weights),
        'weights_sha256': hashlib.sha256(weights.read_bytes()).hexdigest(),
        'runs': {},
    }
    for label, run in runs.items():
        root = run['root']
        view = output / (label + '-input')
        (view / 'spec_all').mkdir(parents=True)
        (view / 'metadata.txt').write_text((root / 'metadata.txt').read_text())
        for point in points:
            (view / 'spec_all' / point).symlink_to(
                root / 'spec_all' / point, target_is_directory=True)
        report = output / label
        argv = [sys.executable, str(args.processor.resolve() / 'run.py'),
                str(view), '-j', str(weights), '--out-dir', str(report),
                '--tag', label]
        with (output / (label + '.log')).open('w') as log:
            subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT,
                           check=True)
        manifest['runs'][label] = {
            'archive': str(root), 'metadata': run['metadata'],
            'configuration': run['configuration'],
            'score_csv': str(report / (label + '-score.csv')),
        }
        print(f'{label}: verified {len(points)} successful points; '
              f'score CSV: {report / (label + "-score.csv")}', flush=True)
    (output / 'comparison-inputs.json').write_text(
        json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
