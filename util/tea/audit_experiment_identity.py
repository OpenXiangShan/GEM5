#!/usr/bin/env python3
"""Verify an experiment's recorded source and binary against its frozen build."""
import argparse
import hashlib
import json
import shlex
from pathlib import Path


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def completed_run_checks(manifest, folder):
    complete = json.loads((folder / 'complete.json').read_text())
    records = complete['results']
    expected = {(point, variant) for point in manifest['points']
                for variant in manifest['variants']}
    checks = {
        'completed_run_matrix_matches_manifest': len(records) == len(expected)
        and {(record['point'], record['variant']) for record in records}
        == expected,
        'completed_guests_returned_zero': complete['passed'] is True
        and all(record['returncode'] == 0 for record in records),
        'completed_records_match_folders': True,
        'completed_binary_paths_match_manifest': True,
        'completed_logged_commands_match_records': True,
    }
    for record in records:
        run_folder = folder / (record['point'] + '-' + record['variant'])
        stored = json.loads((run_folder / 'run.json').read_text())
        checks['completed_records_match_folders'] &= stored == record
        checks['completed_binary_paths_match_manifest'] &= (
            record['command'][0] == manifest['binary'])
        commands = [shlex.split(line.removeprefix('command line: '))
                    for line in (run_folder / 'run.log').read_text().splitlines()
                    if line.startswith('command line: ')]
        checks['completed_logged_commands_match_records'] &= (
            len(commands) == 1 and commands[0] == record['command'])
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--source-patch', type=Path, required=True)
    parser.add_argument('--variant', choices=['fast', 'opt'], default='fast')
    parser.add_argument(
        '--completed-runs', action='store_true',
        help='Also bind completed flat point-policy runs to their startup logs')
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    build = json.loads(args.build.read_text())
    binary = Path(manifest['binary'])
    binary_sha = sha256(binary)
    source_sha = sha256(args.source_patch)
    checks = {
        'build_completed_successfully': build['complete'] is True
        and build['returncode'] == 0,
        'manifest_binary_matches_file': manifest['binary_sha256'] == binary_sha,
        'build_binary_matches_file': build['variants'][args.variant][
            'binary_sha256'
        ] == binary_sha,
        'manifest_source_matches_patch': manifest['source_patch_sha256']
        == source_sha,
        'build_source_matches_patch': build['source_patch_sha256'] == source_sha,
    }
    if args.completed_runs:
        checks.update(completed_run_checks(manifest, args.manifest.parent))
    passed = all(checks.values())
    print(json.dumps({
        'passed': passed,
        'manifest': str(args.manifest),
        'build': str(args.build),
        'source_patch': str(args.source_patch),
        'binary': str(binary),
        'binary_sha256': binary_sha,
        'source_patch_sha256': source_sha,
        'checks': checks,
    }, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
