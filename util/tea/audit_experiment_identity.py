#!/usr/bin/env python3
"""Verify an experiment's recorded source and binary against its frozen build."""
import argparse
import hashlib
import json
from pathlib import Path


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--source-patch', type=Path, required=True)
    parser.add_argument('--variant', choices=['fast', 'opt'], default='fast')
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
