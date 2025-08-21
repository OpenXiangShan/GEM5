#!/usr/bin/env python3
import argparse, json, os, sys, re
from args_template import CliffArgsTemplate

def load_json(path: str):
    try:
        with open(path, "r", encoding="utf-8") as f:
            raw = f.read()
    except FileNotFoundError:
        sys.exit(f"[Error] JSON file not found: {path}")
    if not raw.strip():
        sys.exit("[Error] JSON file is empty.")
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as e:
        print(f"[Error] Invalid JSON: {e}", file=sys.stderr)
        print(f"[Hint] First 200 chars: {raw[:200]!r}", file=sys.stderr)
        sys.exit(3)
    if not isinstance(data, list) or not data:
        sys.exit("[Error] JSON must be a non-empty list.")
    return data

def pick_record(records, file_exact=None):
    if file_exact is None:
        return records[0]
    for r in records:
        if str(r.get("file")) == file_exact:
            return r
    sys.exit(f"[Error] file='{file_exact}' not found in JSON.")

def main():
    ap = argparse.ArgumentParser(description="Replace function body in existing .py file from JSON.")
    ap.add_argument("--params-json", required=True)
    ap.add_argument("--file", default=None)
    ap.add_argument("--out-py", required=True, help="Existing .py file path to modify")
    ap.add_argument("--function-name", default="autoCalibrateParams")
    args = ap.parse_args()

    # 1. Load JSON and pick the desired record
    records = load_json(args.params_json)
    rec = pick_record(records, args.file)
    params = {k: v for k, v in rec.items() if k != "file"}

    # 2. Render the function body from the templat
    tmpl = CliffArgsTemplate()
    lines = tmpl.render(params=params)
    new_func_lines = []
    new_func_lines.append(f"def {args.function_name}(args, system):\n")
    new_func_lines.append("    for cpu in system.cpu:\n")
    for line in lines:
        code_line = line.strip()
        new_func_lines.append(f"        {code_line}\n")
    new_func_code = "".join(new_func_lines)

    # 3. Read the existing Python file
    if not os.path.exists(args.out_py):
        sys.exit(f"[Error] File not found: {args.out_py}")
    with open(args.out_py, "r", encoding="utf-8") as f:
        content = f.read()

    # 4. Use regex to replace the existing function definition and body
    #    - Match from 'def <function_name>(' to the next non-indented line or end of file
    pattern = rf"(^def\s+{re.escape(args.function_name)}\(.*?\):\n)(?:[ \t].*\n)*"
    if re.search(pattern, content, flags=re.MULTILINE):
        # Function found → replace it entirely
        content = re.sub(pattern, new_func_code + "\n", content, flags=re.MULTILINE)
        print(f"[OK] Replaced function '{args.function_name}' in {args.out_py}")
    else:
        # Function not found → append to the end of the file
        content += "\n\n" + new_func_code + "\n"
        print(f"[OK] Appended new function '{args.function_name}' to {args.out_py}")

    # 5. Write back the modified file
    with open(args.out_py, "w", encoding="utf-8") as f:
        f.write(content)

if __name__ == "__main__":
    main()

