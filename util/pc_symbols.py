"""Resolve exact RISC-V instruction PCs using workload ELF files."""

import json
from pathlib import Path
import re
import shutil
import struct
import subprocess


def objdump_tool(requested=None):
    return (
        requested
        or shutil.which("riscv64-unknown-linux-gnu-objdump")
        or shutil.which("riscv64-linux-gnu-objdump")
    )


def symbolize(pcs, elfs, objdump=None):
    """Match instruction boundaries exactly; never use the preceding PC."""
    if not elfs:
        return {}
    tool = objdump_tool(objdump)
    if not tool:
        raise ValueError(
            "instruction labels require RISC-V objdump; use --objdump PATH"
        )
    symbols = {}
    wanted = {int(pc, 16): pc for pc in pcs}
    for elf in elfs:
        # Executable LOAD ranges avoid decoding data or unrelated ELF images.
        with Path(elf).open("rb") as source:
            header = source.read(64)
            if header[:6] != b"\x7fELF\x02\x01":
                raise ValueError(f"{elf}: expected a little-endian ELF64 file")
            offset = struct.unpack_from("<Q", header, 32)[0]
            size, count = struct.unpack_from("<HH", header, 54)
            spans = []
            for index in range(count):
                source.seek(offset + index * size)
                kind, flags, _, address, _, length, _, _ = struct.unpack(
                    "<IIQQQQQQ", source.read(56)
                )
                if kind == 1 and flags & 1:
                    spans.append((address, address + length))
        selected = {
            pc
            for pc in wanted
            if wanted[pc] not in symbols and any(a <= pc < b for a, b in spans)
        }
        if not selected:
            continue
        # Start at section boundaries, not at each requested PC: starting in
        # the middle of a variable-length instruction can invent a decoding.
        command = [
            tool,
            "-d",
            "--no-show-raw-insn",
            "--demangle",
            f"--stop-address={max(selected) + 4}",
            str(elf),
        ]
        with subprocess.Popen(
            command, stdout=subprocess.PIPE, text=True
        ) as process:
            function = ""
            for line in process.stdout:
                header = re.match(r"^\s*[0-9a-fA-F]+ <(.*)>:$", line)
                if header:
                    function = header[1]
                instruction = re.match(r"^\s*([0-9a-fA-F]+):\s+(.+)$", line)
                if instruction and int(instruction[1], 16) in selected:
                    pc = wanted[int(instruction[1], 16)]
                    symbols[pc] = (function, " ".join(instruction[2].split()))
            if process.wait():
                raise subprocess.CalledProcessError(
                    process.returncode, command
                )
    return symbols


def pc_labels(pcs, elfs=(), objdump=None):
    symbols = symbolize(pcs, elfs, objdump)
    return [
        f"{pc}  {symbols[pc][1]}\n{symbols[pc][0]}"
        if pc in symbols
        else f"{pc}  [unmapped]"
        if elfs
        else pc
        for pc in pcs
    ]


def profile_elfs(config, output):
    """Find the actual SPEC application in the checkpoint manifest's rootfs."""
    checkpoint = Path(config.get("system", "gcpt_file", fallback=""))
    checkpoint_root = next(
        (p for p in checkpoint.parents if p.name == "checkpoint"), None
    )
    if checkpoint_root is None:
        return []
    profile = checkpoint_root.parent
    workload = checkpoint.relative_to(checkpoint_root).parts[0]
    manifest = profile / "manifest" / f"{workload}.json"
    if not manifest.is_file():
        return []
    metadata = json.loads(manifest.read_text())
    command_file = profile / "cmd" / f"{workload}.run.sh"
    rootfs = profile / metadata["initramfs"]["file"]
    if not command_file.is_file() or not rootfs.is_file():
        return []
    executables = {
        "spec/" + name
        for name in re.findall(
            r"\bspec_cmd=['\"]\./([^\s'\"]+)", command_file.read_text()
        )
    }
    candidates = list((profile / "elf").glob("*.elf"))
    elfs = []
    with rootfs.open("rb") as stream:
        while True:
            header = stream.read(110)
            if not header:
                break
            if header[:6] not in (b"070701", b"070702"):
                raise ValueError(
                    f"{manifest}: rootfs must be a newc CPIO archive"
                )
            size, name_size = int(header[54:62], 16), int(header[94:102], 16)
            name = stream.read(name_size).rstrip(b"\0").decode()
            stream.seek((-(110 + name_size)) % 4, 1)
            if name == "TRAILER!!!":
                break
            if name in executables:
                magic = stream.read(min(4, size))
                if magic == b"\x7fELF":
                    image = magic + stream.read(size - 4)
                    match = next(
                        (
                            p
                            for p in candidates
                            if p.stat().st_size == size
                            and p.read_bytes() == image
                        ),
                        None,
                    )
                    if match is None:
                        match = output / "elf" / Path(name).name
                        match.parent.mkdir(parents=True, exist_ok=True)
                        match.write_bytes(image)
                    elfs.append(match)
                else:
                    stream.seek(size - len(magic), 1)
            else:
                stream.seek(size, 1)
            stream.seek((-size) % 4, 1)
    kernel = profile / metadata["kernel"]["vmlinux"]
    if kernel.is_file():
        elfs.append(kernel)
    return elfs
