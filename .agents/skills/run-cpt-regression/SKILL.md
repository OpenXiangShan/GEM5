---
name: run-cpt-regression
description: "仅负责批量运行 gem5 checkpoint（1次或2次）。不做任何分析。"
---

# 批量 CPT 运行技能（仅运行）

## 何时使用
- 你只想批量跑 checkpoint / 小测试。
- 你希望 run 与 analysis 完全解耦。

## 核心原则
- 这个 skill **不做分析**。
- 只产出运行目录、`stats.txt`、`gem5.stdout`、`gem5.stderr`。

## 入口脚本
- `.agents/skills/run-cpt-regression/scripts/run_cpt_back.py`

## 典型用法
批量跑（默认 ref+opt）：

```bash
python3 .agents/skills/run-cpt-regression/scripts/run_cpt_back.py \
  --debug-dir /tmp/debug/tage-new8
```

仅跑 opt（跳过 ref）：

```bash
python3 .agents/skills/run-cpt-regression/scripts/run_cpt_back.py \
  --debug-dir /tmp/debug/tage-new8 \
  --skip-ref
```

仅跑指定切片：

```bash
python3 .agents/skills/run-cpt-regression/scripts/run_cpt_back.py \
  --debug-dir /tmp/debug/tage-new8 \
  --slices 2fetch coremark10
```

带参数运行某个切片, 使用-P：
```bash
GCBV_REF_SO=<path/to/riscv64-nemu-interpreter-so> \
./build/RISCV/gem5.opt ./configs/example/kmhv3.py \
    --raw-cpt \
    --generic-rv-cpt=<path/to/raw_checkpoint.bin> \
    -P "system.cpu[0].branchPred.mgsc.enabled=True"
```

## 后续分析
请使用另一个 skill：`frontend-pmu-analysis`。
