#!/usr/bin/env python3
"""Build a software WFI-loop restorer without modifying LibCheckpoint sources."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def build(source, output, cross_compile, mode):
    source = source.resolve()
    output = output.resolve()
    if output == source or source in output.parents:
        raise ValueError("output must be outside the original source tree")
    target = output / "source"
    original = source / "src/decode_restore.c"
    text = original.read_text()
    anchor = "  if (should_jump_to_mepc) {\n    restore_int_vector_jump"
    if text.count(anchor) != 1 or "global_atomic_restore_counter" not in text:
        raise ValueError("unsupported LibCheckpoint restore/synchronization layout")
    output.mkdir(parents=True, exist_ok=True)
    if target.exists():
        raise ValueError(f"refusing to overwrite an existing build tree: {target}")
    shutil.copytree(
        source, target,
        ignore=shutil.ignore_patterns(".git", "build", "export_include", "CONFIG.mk"),
    )
    # This site is after the shared restore barrier and before architectural
    # integer restoration/mret. T0 takes the original path unchanged. T1 keeps
    # machine privilege and uses its existing architectural rename mappings.
    hook = '''  if (cpu_id == 1) {
    __asm__ volatile (
      "csrw mie, zero\\n"
      "csrci mstatus, 8\\n"
      "fence rw, rw\\n"
      ".option push\\n"
      ".option norvc\\n"
      ".global smt_test_wfi_loop\\n"
      "smt_test_wfi_loop: wfi\\n"
      "j smt_test_wfi_loop\\n"
      ".option pop\\n"
      : : : "memory");
    __builtin_unreachable();
  }

'''
    (target / "src/decode_restore.c").write_text(text.replace(anchor, hook + anchor))
    subprocess.run(["bash", "configure", "--mode=" + mode], cwd=target, check=True)
    subprocess.run(
        ["make", "-j8", "CROSS_COMPILE=" + cross_compile,
         "LDFLAGS=-no-pie -Wl,--build-id=none"], cwd=target, check=True,
    )
    binary = target / "build/gcpt.bin"
    if binary.stat().st_size > 2**21:
        raise ValueError("restorer exceeds gem5's 2 MiB multi-context limit")
    result = output / "gcpt-wfi.bin"
    shutil.copy2(binary, result)
    shutil.copy2(target / "build/gcpt", output / "gcpt-wfi.elf")
    nm = subprocess.check_output(
        [cross_compile + "nm", "-n", str(output / "gcpt-wfi.elf")], text=True,
    )
    symbols = {line.split()[-1]: int(line.split()[0], 16)
               for line in nm.splitlines() if len(line.split()) == 3}
    wfi_offset = symbols["smt_test_wfi_loop"]
    data = result.read_bytes()
    if symbols["place_of_gcpt_size"] != 0 or data[0] & 0x7f != 0x6f:
        raise ValueError("restorer must start with the header JAL at offset zero")
    if data[wfi_offset:wfi_offset + 4] != bytes.fromhex("73005010"):
        raise ValueError("smt_test_wfi_loop does not point to a WFI instruction")
    version = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        text=True, capture_output=True,
    )
    manifest = {
        "experiment": "wfi-loop",
        "source": str(source),
        "source_commit": version.stdout.strip() if version.returncode == 0 else None,
        "source_decode_restore_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
        "patched_decode_restore_sha256": hashlib.sha256(
            (target / "src/decode_restore.c").read_bytes()).hexdigest(),
        "binary": str(result), "mode": mode, "wfi_hart": 1,
        "loop_hex": "730050106ff0dfff",
        "wfi_offset": wfi_offset,
        "wfi_pc": 0x80000000 + wfi_offset,
        "binary_sha256": hashlib.sha256(result.read_bytes()).hexdigest(),
        "cross_compile": cross_compile,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="LibCheckpoint source with initialized nanopb")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cross-compile", default="riscv64-linux-gnu-")
    parser.add_argument("--mode", choices=["dual_core", "normal"], default="dual_core")
    args = parser.parse_args()
    build(args.source, args.output, args.cross_compile, args.mode)


if __name__ == "__main__":
    main()
