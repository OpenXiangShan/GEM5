---
name: run-cpt-regression
description: "仅负责批量运行 gem5 / RTL checkpoint 或前端小测试（1次或2次）。不做任何分析。"
---

# 批量 CPT / RTL 运行技能（仅运行）

## 何时使用
- 你只想批量跑 checkpoint / 小测试。
- 你希望 run 与 analysis 完全解耦。

## 核心原则
- 这个 skill **不做分析**。
- 只产出运行目录和原始日志，如 `stats.txt`、`gem5.stdout`、`gem5.stderr`、`rtl.stdout`、`rtl.stderr`。

## 入口脚本
- `.codex/skills/run-cpt-regression/scripts/run_cpt_back.py`

## 典型用法
批量跑 gem5（默认 ref+opt）：

```bash
python3 .codex/skills/run-cpt-regression/scripts/run_cpt_back.py \
  --debug-dir /tmp/debug/tage-new8
```

仅跑 gem5 opt（跳过 ref）：

```bash
python3 .codex/skills/run-cpt-regression/scripts/run_cpt_back.py \
 --debug-dir /tmp/debug/tage-new8 \
  --skip-ref
```

仅跑 RTL：

```bash
python3 .codex/skills/run-cpt-regression/scripts/run_cpt_back.py \
  --backend rtl \
  --debug-dir /tmp/debug/tage-rtl \
  --rtl-no-diff
```

同时跑 gem5 和 RTL：

```bash
python3 .codex/skills/run-cpt-regression/scripts/run_cpt_back.py \
  --backend both \
  --debug-dir /tmp/debug/tage-ab \
  --skip-ref \
  --rtl-no-diff
```

仅跑指定切片：

```bash
python3 .codex/skills/run-cpt-regression/scripts/run_cpt_back.py \
  --debug-dir /tmp/debug/tage-new8 \
  --slices 2fetch coremark10
```

带参数运行某个 gem5 切片，使用 `-P`：
```bash
GCBV_REF_SO=<path/to/riscv64-nemu-interpreter-so> \
./build/RISCV/gem5.opt ./configs/example/kmhv3.py \
    --raw-cpt \
    --generic-rv-cpt=<path/to/raw_checkpoint.bin> \
    -P "system.cpu[0].branchPred.mgsc.enabled=True"
```

RTL 默认参数：

- `--rtl-warmup-instr 0`
- `--rtl-max-instr 0`
- `--rtl-stat-cycles 0`
- 默认走 `/nfs/home/yanyue/workspace/xs-env/XiangShan/build/emu`
- 当 `warmup/max-instr/stat-cycles` 为 `0` 时，脚本不会额外附带这些参数
- 可通过 `--rtl-arg` 重复追加额外 `emu` 参数

## 后续分析
请使用另一个 skill：`frontend-pmu-analysis`。
