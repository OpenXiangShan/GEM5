#!/usr/bin/env python3
import os
import re
import sys
import json
import csv
import argparse
from typing import Any, Dict, Optional, List

# Target case class name
CASE_CLASS_NAME = r"XSCoreParameters"

# Parameters to extract: key -> type ("int" or "bool")
TARGET_KEYS: Dict[str, str] = {
    "XLEN": "int",
    "VLEN": "int",
    "HartId": "int",
    "HasPrefetch": "bool",
    "FetchWidth": "int",
    "FtqSize": "int",
    "DecodeWidth": "int",
    "RenameWidth": "int",
    "CommitWidth": "int",
    "VirtualLoadQueueSize": "int",
    "StoreQueueSize": "int",
    "StoreBufferSize": "int",
    "StoreBufferThreshold": "int",
    "LoadQueueRARSize": "int",
    "LoadQueueRAWSize": "int",
    "RobSize": "int",
    "intPreg": "int",
    "fpPreg": "int",
    "vfPreg": "int",
}

def strip_comments(code: str) -> str:
    """Remove /* ... */ block comments and // line comments."""
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    code = re.sub(r"//.*?$", "", code, flags=re.M)
    return code

def find_params_block(code: str) -> Optional[str]:
    """Locate the parameter block inside case class XSCoreParameters(...)."""
    m = re.search(rf"case\s+class\s+{CASE_CLASS_NAME}\s*\(", code)
    if not m:
        return None
    i = m.end()
    depth = 1
    while i < len(code) and depth > 0:
        c = code[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        i += 1
    if depth != 0:
        return None
    return code[m.end(): i-1]  # exclude the final ')'

def normalize_value(raw: str) -> str:
    """Trim whitespace, remove trailing commas, and strip outer parentheses."""
    v = raw.strip()
    if v.endswith(","):
        v = v[:-1].rstrip()
    while v.startswith("(") and v.endswith(")"):
        v = v[1:-1].strip()
    return v

# New helper: scan the right-hand side after '=' until a top-level comma or ')'
# This avoids premature cutoff when the value spans multiple lines or contains nested calls.
def grab_rhs_until_top_comma(s: str, start: int) -> str:
    depth = 0
    j = start
    while j < len(s):
        c = s[j]
        if c == '(':
            depth += 1
        elif c == ')':
            if depth > 0:
                depth -= 1
            else:
                # Reached a top-level closing parenthesis (should not happen for RHS, but keep for safety)
                break
        elif c == ',' and depth == 0:
            break
        j += 1
    return s[start:j].strip()


def parse_scalar(value: str, expected_type: str) -> Optional[Any]:
    """Convert the raw string value to the expected type (int or bool)."""
    v = value.strip()
    if expected_type == "bool":
        if re.fullmatch(r"true", v, flags=re.I):
            return True
        if re.fullmatch(r"false", v, flags=re.I):
            return False
        return None
    if expected_type == "int":
        m = re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)", v)
        if m:
            try:
                return int(m.group(1), 0)
            except ValueError:
                return None
        return None
    return None

def extract_values(params_block: str, targets: Dict[str, str]) -> Dict[str, Optional[Any]]:
    """Extract default values of the target keys from the parameter block."""
    results: Dict[str, Optional[Any]] = {k: None for k in targets}
    for key, expected_type in targets.items():
        # Locate "key [: type] = " anchor
        anchor = re.search(rf"""\b{key}\b\s*(?::\s*[\w\[\]]+)?\s*=\s*""", params_block)
        if not anchor:
            continue
        start = anchor.end()

        # Use bracket-depth scanning to grab the complete RHS,
        # which may include nested parentheses, commas, and line breaks.
        raw = grab_rhs_until_top_comma(params_block, start)
        raw = normalize_value(raw)

        # Special handling: if RHS is an object initialization like IntPregParams(...),
        # extract the numEntries field.
        if key == "intPreg" or key == "fpPreg" or key == "vfPreg":
            m_ne = re.search(r"""numEntries\s*=\s*(0x[0-9a-fA-F]+|\d+)""", raw)
            if m_ne:
                try:
                    results[key] = int(m_ne.group(1), 0)
                    continue
                except ValueError:
                    pass  # fallback to scalar parsing if conversion fails

        # Default: parse as scalar int/bool
        results[key] = parse_scalar(raw, expected_type)
    return results

def process_file(path: str) -> Optional[Dict[str, Optional[Any]]]:
    """Read a Parameters.scala file and extract the target parameters."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            code = f.read()
    except Exception:
        return None
    code_nc = strip_comments(code)
    params_block = find_params_block(code_nc)
    if not params_block:
        return None
    return extract_values(params_block, TARGET_KEYS)

def parse_args():
    p = argparse.ArgumentParser(description="Extract XSCoreParameters from Parameters.scala")
    p.add_argument("directory", help="Root dir to search")
    p.add_argument("--format", "-f", choices=["json", "csv", "text"], default="json",
                   help="Output format (default: json)")
    p.add_argument("--output", "-o", help="Write to file instead of stdout")
    return p.parse_args()

def ensure_parent(path: Optional[str]) -> None:
    if not path:
        return
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)

def main():
    args = parse_args()
    base = args.directory
    fmt = args.format
    out_path = args.output

    results: List[Dict[str, Any]] = []
    for root, _, files in os.walk(base):
        for fn in files:
            if fn == "Parameters.scala":
                fp = os.path.join(root, fn)
                vals = process_file(fp)
                if vals is None:
                    continue
                entry = {"file": fp}
                entry.update(vals)
                results.append(entry)

    # No results -> stderr + non-zero, do NOT emit fake JSON
    if not results:
        print(f"No Parameters.scala with XSCoreParameters(...) found under: {base}", file=sys.stderr)
        sys.exit(2)

    if fmt == "json":
        payload = json.dumps(results, indent=2, ensure_ascii=False)
        if out_path:
            ensure_parent(out_path)
            with open(out_path, "w", encoding="utf-8") as f:
                f.write(payload + "\n")
        else:
            print(payload)
    elif fmt == "csv":
        fieldnames = ["file"] + list(TARGET_KEYS.keys())
        if out_path:
            ensure_parent(out_path)
            with open(out_path, "w", encoding="utf-8", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=fieldnames)
                writer.writeheader()
                for row in results:
                    writer.writerow(row)
        else:
            writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames)
            writer.writeheader()
            for row in results:
                writer.writerow(row)
    else:  # text
        lines = []
        for r in results:
            kv = " ".join(f"{k}={r.get(k)}" for k in TARGET_KEYS)
            lines.append(f"{r['file']}: {kv}")
        if out_path:
            ensure_parent(out_path)
            with open(out_path, "w", encoding="utf-8") as f:
                f.write("\n".join(lines) + "\n")
        else:
            print("\n".join(lines))

if __name__ == "__main__":
    main()
