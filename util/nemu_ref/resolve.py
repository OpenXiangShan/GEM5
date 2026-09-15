#!/usr/bin/env python3
"""Resolve and verify a variant of the pinned GEM5 NEMU release."""

import argparse
import hashlib
import json
from pathlib import Path


HERE = Path(__file__).resolve().parent
LOCK = json.loads((HERE / "lock.json").read_text())
RELEASE_ROOT = Path("/nfs/home/share/gem5_ci/ref/releases")
SO_NAME = "riscv64-nemu-interpreter-so"


def ref_path(variant, root=RELEASE_ROOT):
    if variant not in LOCK["variants"]:
        raise ValueError(f"Unknown NEMU variant: {variant}")
    return Path(root) / LOCK["release"] / variant / SO_NAME


def verify_ref(variant, root=RELEASE_ROOT):
    path = ref_path(variant, root)
    manifest = json.loads((path.parent / "manifest.json").read_text())
    for key, expected in (
        ("commit", LOCK["commit"]), ("release", LOCK["release"]),
        ("variant", variant), ("register_size", 1376),
        ("dependencies", LOCK["dependencies"]),
        ("lock_sha256", hashlib.sha256((HERE / "lock.json").read_bytes()).hexdigest()),
    ):
        if manifest[key] != expected:
            raise ValueError(f"NEMU manifest mismatch: {path}: {key}")
    for file, key in ((path, "sha256"),
                      (Path(str(path) + ".config"), "config_sha256")):
        if hashlib.sha256(file.read_bytes()).hexdigest() != manifest[key]:
            raise ValueError(f"NEMU checksum mismatch: {file}")
    fragments = ["common.config", f"{variant}.config"]
    if variant in ("normal", "normal-dedup"):
        fragments.append("scalar.config")
    for name in fragments:
        if hashlib.sha256((HERE / name).read_bytes()).hexdigest() != \
                manifest["fragment_sha256"][name]:
            raise ValueError(f"NEMU config fragment mismatch: {name}")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("variant", choices=LOCK["variants"])
    parser.add_argument("--root", type=Path, default=RELEASE_ROOT)
    args = parser.parse_args()
    print(verify_ref(args.variant, args.root))


if __name__ == "__main__":
    main()
