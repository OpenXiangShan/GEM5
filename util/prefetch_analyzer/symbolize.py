"""Resolve observed PCs against an explicitly supplied workload ELF."""

import subprocess
from pathlib import Path

from parse_trace import digest, require


def symbolize_pcs(pcs, elf, addr2line):
    elf = Path(elf).resolve()
    addresses = sorted({row["pc"] for row in pcs if row["pc"]})
    command = [addr2line, "-f", "-C", "-e", str(elf)]
    lines = (
        subprocess.check_output(
            command, input="\n".join(addresses) + "\n", text=True
        ).splitlines()
        if addresses
        else []
    )
    require(len(lines) == 2 * len(addresses), "Unexpected addr2line output")
    symbols = dict(zip(addresses, zip(lines[::2], lines[1::2])))
    for row in pcs:
        function, location = symbols.get(row["pc"], ("??", "??:?"))
        row["function"] = function if function != "??" else None
        row["source_location"] = location
    return {"elf": str(elf), "sha256": digest(elf), "command": command}
