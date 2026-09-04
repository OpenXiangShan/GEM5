---
name: ci-perf-analysis
description: 用于从 GitHub Actions 的 gem5 性能 CI 中定位 summary、score.txt 和归档目录，并结合本地 gem5_data_proc 对 spec06/spec17 结果做 weighted score、benchmark 子项对比和通用 stats 归因。适用于用户给出 run URL/run id、commit、workflow run，或要求分析 CI 跑分变化来源时。
---

# CI 性能分析

## 概览

这个 skill 处理以下链路：

1. 从 CI run 的所有 jobs 中定位真实性能归档目录和 `score.txt`。
2. 用本地 `gem5_data_proc/run.py` 生成 CSV、weighted CSV 和 score CSV。
3. 对比 benchmark 级变化；必要时再下钻 `stats.txt`。

## 1. 定位归档

优先使用仓库内脚本：

```bash
python3 .agents/skills/ci-perf-analysis/scripts/ci_perf_info.py \
  https://github.com/OpenXiangShan/GEM5/actions/runs/<run_id>
```

脚本会遍历 run 的 jobs，并兼容当前和旧版 workflow 的归档日志格式。输出包括：

- `job_id` 和 `job_name`
- `archive_path`
- `spec_all`
- 本地可访问时的 `score.txt` 尾部

不要默认取第一个 job，也不要从日志中的示例文本推断归档位置。

非默认仓库可使用 `--repo <owner/repo>`。

## 2. 选择 gem5_data_proc

路径优先级：

1. 用户明确给出的路径
2. 环境变量 `GEM5_DATA_PROC_HOME`
3. 本机常见默认值 `/nfs/home/yanyue/workspace/gem5_data_proc`

```bash
export GEM5_DATA_PROC_HOME="${GEM5_DATA_PROC_HOME:-/nfs/home/yanyue/workspace/gem5_data_proc}"
test -f "$GEM5_DATA_PROC_HOME/run.py"
```

若以上路径均不可用，先说明缺失并征得用户同意，再安装或 clone；不要把个人 home 路径当成所有机器的前提。

## 3. 处理归档

始终使用步骤 1 返回的完整 `<archive_path>`，不要手写固定的 benchmark 套件目录：

```bash
python3 "$GEM5_DATA_PROC_HOME/run.py" <archive_path> \
  --out-dir /tmp/gem5_proc_runA \
  --tag runA
```

常用输出：

- `<tag>.csv`：原始 point 或 benchmark 聚合结果
- `<tag>-weighted.csv`：按权重聚合的 benchmark 统计
- `<tag>-score.csv`：score、time 和 coverage

若 `run.py` 在个别 point 上报数据处理异常，但已生成 `score.txt` 或部分 CSV，要明确标注数据缺口；不要把部分输出说成完整成功。

## 4. 比较两个 run

```bash
python3 .agents/skills/ci-perf-analysis/scripts/ci_perf_info.py <runA>
python3 .agents/skills/ci-perf-analysis/scripts/ci_perf_info.py <runB>

python3 "$GEM5_DATA_PROC_HOME/run.py" <archiveA> \
  --out-dir /tmp/gem5_proc_A --tag A
python3 "$GEM5_DATA_PROC_HOME/run.py" <archiveB> \
  --out-dir /tmp/gem5_proc_B --tag B
```

分析顺序：

1. 固定 commit、workflow、配置和 workload 口径。
2. 比较总 score、time 和 coverage。
3. 按 benchmark 的 score/time delta 排序。
4. 从 weighted CSV 查看前端、后端、内存和分支等指标。
5. 只对重点 benchmark 下钻 `<archive_path>/spec_all/<slice>/m5out/stats.txt`。

## 输出要求

回答优先给出：

1. run、commit、配置和 workload 差异
2. 总分变化
3. 主要收益和回退 benchmark
4. 相关 stats 证据
5. 根因判断与未决风险

区分事实、推断和数据缺口。尤其不要把“run 已创建”写成“性能 CI 已通过”。

## 资源

- `scripts/ci_perf_info.py`：从 run URL 或 ID 定位归档路径并打印 score 尾部。
