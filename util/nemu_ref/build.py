#!/usr/bin/env python3
"""Build pinned NEMU variants without modifying the source checkout."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


HERE = Path(__file__).resolve().parent
SO_NAME = "riscv64-nemu-interpreter-so"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def export_tree(repository, commit, destination):
    destination.mkdir(parents=True, exist_ok=True)
    # git archive ignores local edits and generated build products.
    with subprocess.Popen(
        ["git", "-C", str(repository), "archive", commit],
        stdout=subprocess.PIPE,
    ) as archive:
        subprocess.run(
            ["tar", "-x", "-C", str(destination)],
            stdin=archive.stdout, check=True,
        )
        archive.stdout.close()
        if archive.wait():
            raise RuntimeError(f"Cannot export {repository} at {commit}")


def build(source, output, lock, variant, jobs):
    target = output / variant
    target.mkdir(parents=True, exist_ok=False)
    work = target / "source"
    export_tree(source, lock["commit"], work)
    for path, commit in lock["dependencies"].items():
        export_tree(source / path, commit, work / path)

    fragments = [HERE / "common.config"]
    if variant in ("normal", "normal-dedup"):
        fragments.append(HERE / "scalar.config")
    fragments.append(HERE / f"{variant}.config")
    config = work / "configs" / "gem5-ci_defconfig"
    base = work / "configs" / lock["base_defconfig"]
    # NEMU's own Kconfig parser resolves the appended overrides.
    config.write_text(base.read_text() + "\n" + "\n".join(
        fragment.read_text() for fragment in fragments
    ))
    env = dict(os.environ, NEMU_HOME=str(work), __NOT_DEFINED="1")
    with (target / "build.log").open("w") as log:
        subprocess.run(["make", "gem5-ci_defconfig"], cwd=work, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)

    actual = set((work / ".config").read_text().splitlines())
    for fragment in fragments:
        for line in fragment.read_text().splitlines():
            if line.startswith("CONFIG_") and line not in actual:
                raise RuntimeError(f"Kconfig did not retain {line}")
            if line.startswith("# CONFIG_") and line.endswith(" is not set"):
                # Hidden disabled symbols may be omitted from .config.
                key = line.split()[1] + "="
                if any(value.startswith(key) for value in actual):
                    raise RuntimeError(f"Kconfig did not retain {line}")

    with (target / "build.log").open("a") as log:
        subprocess.run(["make", f"-j{jobs}"], cwd=work, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)

    so = target / SO_NAME
    shutil.copy2(work / "build" / SO_NAME, so)
    shutil.copy2(work / ".config", target / f"{SO_NAME}.config")
    shutil.copy2(work / "include/generated/autoconf.h",
                 target / f"{SO_NAME}.autoconf.h")
    # Probe in a fresh process so different variants cannot share symbols.
    probe = (
        "import ctypes, sys; "
        "lib = ctypes.CDLL(sys.argv[1]); "
        "print(ctypes.c_uint.in_dll(lib, 'DIFFTEST_REG_SIZE').value)"
    )
    reg_size = int(subprocess.check_output(
        [os.sys.executable, "-c", probe, str(so)], text=True,
    ))
    if reg_size != 1376:
        raise RuntimeError(f"Unexpected register ABI size: {reg_size}")
    metadata = {
        "release": lock["release"], "commit": lock["commit"],
        "variant": variant, "dependencies": lock["dependencies"],
        "register_size": reg_size,
        "sha256": sha256(so),
        "config_sha256": sha256(target / f"{SO_NAME}.config"),
        "lock_sha256": sha256(HERE / "lock.json"),
        "fragment_sha256": {p.name: sha256(p) for p in fragments},
        "compiler": subprocess.check_output(
            ["gcc", "--version"], text=True,
        ).splitlines()[0],
    }
    (target / "manifest.json").write_text(
        json.dumps(metadata, indent=2) + "\n"
    )
    print(json.dumps(metadata), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path,
                        help="NEMU checkout with pinned dependency objects")
    parser.add_argument("--output", required=True, type=Path,
                        help="New release directory; variants must not exist")
    parser.add_argument("--variant", action="append",
                        help="Build only this variant (repeatable)")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    lock = json.loads((HERE / "lock.json").read_text())
    variants = args.variant or lock["variants"]
    if args.jobs < 1 or not set(variants) <= set(lock["variants"]):
        parser.error("Invalid jobs or variant")
    for variant in variants:
        build(args.source.resolve(), args.output.resolve(), lock,
              variant, args.jobs)


if __name__ == "__main__":
    main()
