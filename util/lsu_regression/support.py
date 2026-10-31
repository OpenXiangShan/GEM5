"""Small process and content-identity helpers for LSU regressions."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def run_record(command, cwd, log, timeout=180):
    start = time.monotonic()
    with log.open("w") as stream:
        try:
            result = subprocess.run(
                command,
                cwd=cwd,
                stdout=stream,
                stderr=subprocess.STDOUT,
                timeout=timeout,
            )
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = 124
    record = {
        "command": list(map(str, command)),
        "cwd": str(cwd),
        "returncode": code,
        "wall_seconds": time.monotonic() - start,
    }
    write_json(log.with_suffix(".execution.json"), record)
    return record
