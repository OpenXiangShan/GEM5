---
name: frontend-pmu-analysis
description: "仅做 BPU 计数器提取与批量汇总（机器可读 JSON/CSV）。配置文件只需要写原始 stats 计数器名。"
---

# BPU 计数器分析技能（极简）

## 何时使用
- 你已经跑完 gem5，只想批量提取 BPU 相关原始计数器。
- 你不想在脚本里做复杂推导，只要原始值。
- 你需要机器可读结果给后续脚本/表格处理。

## 核心原则
- 只提取原始 counter，不做公式推导。
- 配置文件只写 counter 名称。
- 目录结构不做强约束，递归扫描 `stats.txt`。
- 如果发现某些分支预测错误特别高，还可以看`stats.txt` 同目录的`topMispredictsByBranch.csv`, 记录了哪些分支被错误预测很多。
- 必要时候可以使用 `--enable-bp-db tage` 来打开tage 的trace db 分析。

## 入口脚本
- `.agents/skills/frontend-pmu-analysis/scripts/analyze_bpu_counters.py`

## 默认配置
- `.agents/skills/frontend-pmu-analysis/configs/bpu_counters.txt`

## 用法
```bash
python3 .agents/skills/frontend-pmu-analysis/scripts/analyze_bpu_counters.py \
  --debug-dir /tmp/debug/tage-new8
```

指定自定义计数器文件：

```bash
python3 .agents/skills/frontend-pmu-analysis/scripts/analyze_bpu_counters.py \
  --debug-dir /tmp/debug/tage-new8 \
  --counters-file /path/to/my_counters.txt
```

## 输出
- `bpu_counters_summary.json`
- `bpu_counters_summary.csv`

输出字段只包含：
- case 路径
- stats 路径
- `values`（命中的计数器和值）
- `missing`（缺失计数器）
- `errors`（解析错误）

## 计数器文件格式
推荐 `txt`（每行一个）：

```txt
system.cpu.ipc
system.cpu.commit.branchMispredicts
system.cpu.commit.branches
```

如果要批量分析更多计数器，也可以添加到configs 的txt 中

也支持：
- `yaml`：`counters: [ ... ]` 或直接列表
- `csv`：第一列或 `counter` 列为计数器名
