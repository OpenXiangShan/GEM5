"""Read the final stats footer locally or on an SSH worker via stdin."""

import json
import os
from pathlib import Path
import sys


def stats_state(path):
    try:
        with path.open("rb") as source:
            source.seek(max(0, os.fstat(source.fileno()).st_size - 256))
            lines = source.read().rstrip().splitlines()
    except FileNotFoundError:
        return "missing"
    except OSError as error:
        return f"unreadable ({error.strerror})"
    if lines and lines[-1].strip(b"- ") == b"End Simulation Statistics":
        return "complete"
    return "incomplete"


if __name__ == "__main__":
    print(json.dumps([stats_state(Path(p)) for p in json.load(sys.stdin)]))
