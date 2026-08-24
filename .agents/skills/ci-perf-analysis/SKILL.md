---
name: ci-perf-analysis
description: 用于从 GitHub Actions 的 gem5 性能 CI 中定位 summary、score.txt 和归档目录，并结合本地 gem5_data_proc 对 spec06/spec17 结果做 weighted score、benchmark 子项对比和通用 stats 归因。适用于用户给出 run URL/run id、commit、workflow run，或要求分析 CI 跑分变化来源时。
---

# CI 性能分析

## 概览

这个 skill 只做一条固定链路：

1. 用 `gh` 从 CI run 找到 summary 对应的 `score.txt` 和归档目录。
2. 用本地 `gem5_data_proc/run.py` 把 `spec_all/` 处理成 `csv`、`weighted.csv`、`score.csv`。
3. 对比 benchmark 级收益，并在需要时继续下钻到 `stats.txt` 或其他归档结果。

## 快速开始

### 1. 先拿 summary 和归档目录

优先使用 bundled script：

```bash
python3 .agents/skills/ci-perf-analysis/scripts/ci_perf_info.py \
  https://github.com/OpenXiangShan/GEM5/actions/runs/<run_id>
```

输出会包含：

- `archive_path`
- `spec_all` 目录
- `score.txt` 的最后 42 行

这和 workflow 在 GitHub summary 里展示的内容一致，因为 CI 本身就是把 `score.txt` 的最后 42 行写进 step summary。

### 2. 优先使用本地 `gem5_data_proc`

默认优先使用：

```bash
/nfs/home/yanyue/workspace/gem5_data_proc
```

如果用户本地没有，再执行：

```bash
git clone https://github.com/jensen-yan/gem5_data_proc
# 设置环境变量 
export $GEM5_DATA_PROC_HOME=xxx
```


### 3. 用 `gem5_data_proc` 处理整个归档

```bash
cd $GEM5_DATA_PROC_HOME
python3 run.py /nfs/home/share/gem5_ci/performance_data/spec06-0.3c/<archive_dir> \
  --out-dir /tmp/gem5_proc_runA \
  --tag runA
```

关键输出：

- `<tag>.csv`：point 级或 benchmark 聚合后的原始统计
- `<tag>-weighted.csv`：按权重聚合后的 benchmark 统计
- `<tag>-score.csv`：最终 score/time/coverage

### 4. 对比两个 run

最常见的是比较两次 CI：

```bash
python3 .agents/skills/ci-perf-analysis/scripts/ci_perf_info.py <runA>
python3 .agents/skills/ci-perf-analysis/scripts/ci_perf_info.py <runB>

cd $GEM5_DATA_PROC_HOME
python3 run.py <archiveA> --out-dir /tmp/gem5_proc_A --tag A
python3 run.py <archiveB> --out-dir /tmp/gem5_proc_B --tag B
```

然后用短 Python 片段读取两个 `*-score.csv` / `*-weighted.csv` 做对比。优先关注：

- 总 score 变化
- benchmark 级 `time` / `score` 变化
- 用户关心的 stats 指标变化

## 下钻分析

### 1. 看 benchmark 级收益

- 对两个归档分别运行 `run.py`
- 比较 `*-score.csv` 里的 `time` 和 `score`
- 按 `score_delta_pct` 或 `time_delta_pct` 排序

### 2. 看 stats 指标变化

- 先看 `*-weighted.csv` 里的通用统计
- 如果用户已经给出重点指标，直接围绕这些指标对比
- 如果用户没有指定，优先从 `time`、`cpi`、前端、后端、内存、分支等大类里挑变化最明显的项
- 归因时优先描述“哪些 stats 在变”，再解释这些变化更像支持哪类根因

### 3. 需要时再读归档里的其他文件

- `stats.txt`：看原始统计，位置在
    /nfs/home/share/gem5_ci/performance_data/spec06-0.3c/<archive_dir>/<spec_bmk>/m5out/stats.txt
- `score.txt`：对照 summary
- 其他 CSV 或日志：按用户问题决定是否下钻

## 常用命令

### 已知 run URL，直接拿 archive path

```bash
python3 .agents/skills/ci-perf-analysis/scripts/ci_perf_info.py <run_url_or_id>
```

### 已知 archive path，直接处理

```bash
cd $GEM5_DATA_PROC_HOME
python3 run.py <archive_dir> --out-dir /tmp/gem5_proc --tag run
```

## 输出组织建议

回答这类问题时，优先按下面的顺序组织：

1. commit / run / workflow / 配置差异
2. summary 里的总分变化
3. benchmark 级主要收益和回退项
4. 相关 stats 指标变化
5. 对根因的判断

结论要尽量区分：

- “哪个 benchmark 涨了”
- “哪些 stats 在变”
- “这些 stats 更像支持哪类根因”

## 资源

### scripts/

- `ci_perf_info.py`
  - 输入 run URL 或 run id
  - 输出 archive path、spec_all 路径和 `score.txt` tail

### references/

当前不需要额外参考文件。后续如果这套流程扩展到更多 workflow 或更多统计口径，再新增参考文档。
