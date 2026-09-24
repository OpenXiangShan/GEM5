---
name: ci-perf-analysis
description: 以当前分支与 xs-dev 的根基线为基准，定位 GEM5 性能 CI 归档，比较 benchmark 表现并用 stats 分析变化。
---

# CI 性能分析

## 概览

这个 skill 处理以下链路：

1. 先确定当前分支相对于 `xs-dev` 的根基线 commit，不把同分支的旧 CI run 自动当成基线。
2. 从 CI run 的所有 jobs 中定位真实性能归档目录和 `score.txt`。
3. 用本地 `gem5_data_proc/run.py` 生成 CSV、weighted CSV 和 score CSV。
4. 对比 benchmark 级变化；必要时再下钻 `stats.txt`。

## 0. 基线选择规则（必须遵守）

默认问题是“当前分支相对根 `xs-dev` 的性能变化”。基线必须按下面的顺序确定：

```bash
feature_commit=$(git rev-parse HEAD)
xs_dev_ref=origin/xs-dev
xs_dev_tip=$(git rev-parse "$xs_dev_ref")
root_commit=$(git merge-base "$feature_commit" "$xs_dev_ref")
printf 'feature=%s\nxs-dev-tip=%s\nroot=%s\n' \
  "$feature_commit" "$xs_dev_tip" "$root_commit"
```

- `root_commit` 是当前分支和 `origin/xs-dev` 的共同基点，是默认性能基线。
- `xs_dev_tip` 是最新 `origin/xs-dev`，只有用户明确要求“对比最新 xs-dev”时才作为基线；它不等同于分支根。
- 旧的同分支 CI run（例如 `origin/<feature-branch>` 上较早的 commit）不能作为默认基线。它只能作为额外的时间序列或回归参考，并且必须在输出中标注。
- 如果分支经过 rebase、merge 或 squash，仍以实际 `git merge-base` 结果为准，不用父提交或分支名猜测根基线。

归档优先按 `metadata.txt` 的完整 `commit` 或 `commit_short` 精确匹配 `root_commit`。可用以下方式检查共享归档：

```bash
rg -l "commit(_short)?: ${root_commit:0:10}" \
  /nfs/home/share/gem5_ci/performance_data/*/*/metadata.txt
```

如果没有根基线的归档：

- 不得静默退回同分支旧 run；
- 应明确报告“缺少 root xs-dev CI 归档”，并把任何替代对比标成非严格结果；
- 若用户授权，可对 `root_commit` 触发一次同配置 CI，或者使用本地构建/运行补齐基线。

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

拿到候选归档后，必须先读取其 `metadata.txt`，确认 `commit` 与目标基线/当前提交一致。不要只按时间、分支名或 run 编号选择归档。

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

1. 固定 root baseline commit、feature commit、workflow、配置和 workload 口径。
2. 校验两份 `metadata.txt` 的 `config_path`、`config_name`、`benchmark_type`、`checkpoint_list`、`resolved_extra_args` 和参考模型路径。参考模型字段在新归档中通常是 `ref_so`，旧 H-mode 归档可能使用 `h_ref_so`。
3. 如果参考模型、checkpoint 列表或配置不一致，不能称为严格 A/B；必须在结论中单独说明环境差异。字段缺失时标注数据缺口，不要把缺失值当作相同。
4. 比较总 score、time 和 coverage。
5. 按 benchmark 的 score/time delta 排序。
6. 从 weighted CSV 查看前端、后端、内存和分支等指标。
7. 只对重点 benchmark 下钻 `<archive_path>/spec_all/<slice>/m5out/stats.txt`。

## 输出要求

回答优先给出：

1. root xs-dev 基线 commit、feature commit，以及最新 xs-dev tip（如果不同）
2. 两个 run、配置和 workload 差异
3. 总分变化
4. 主要收益和回退 benchmark
5. 相关 stats 证据
6. 根因判断与未决风险

区分事实、推断和数据缺口。尤其不要把“run 已创建”写成“性能 CI 已通过”。

## 资源

- `scripts/ci_perf_info.py`：从 run URL 或 ID 定位归档路径并打印 score 尾部。
