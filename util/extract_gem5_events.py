#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Moved: use util/trace/extract_trace_events.py instead.
This wrapper preserves compatibility and forwards args.
"""

import runpy
import sys
from pathlib import Path


def main():
    target = Path(__file__).parent / "trace" / "extract_trace_events.py"
    if not target.exists():
        print("Error: util/trace/extract_trace_events.py not found", file=sys.stderr)
        return 1
    runpy.run_path(str(target), run_name="__main__")
    return 0


if __name__ == "__main__":
    sys.exit(main())
