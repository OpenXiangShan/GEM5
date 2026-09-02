# 性能 CI 与 Solver 共用切片配置

## 背景

过去有两份内建切片路径配置：

- `.github/workflows/gem5-perf-template.yml` 维护手动性能测试使用的
  checkpoint list、checkpoint root、聚类配置和算分脚本。
- `util/solver/executor/benchmarks.py` 为 solver 再复制一遍相同路径。

这会让一次切片迁移必须同时修改 workflow 和 solver。漏改任意一处后，普通性能测试与
参数求解会在同一个 `benchmark_type` 下运行不同数据。

## 设计

切片配置的唯一来源现在是：

`util/xs_scripts/perf_benchmarks.py`

```text
util/xs_scripts/perf_benchmarks.py
        |
        +-- CLI --> gem5-perf-template.yml --> GitHub step outputs
        |
        +-- import --> solver/executor/benchmarks.py --> solver workloads
```

性能模板仍然是完整性能测试流程的主入口。共享模块只是把模板原来的 `case` 配置变成
可复用、可单测的 resolver，避免 solver 复制模板数据。

共享字段与消费者的关系如下：

| 共享字段 | 性能 workflow 输出 | Solver 用途 |
| --- | --- | --- |
| `checkpoint_list` | `checkpoint_list` | 枚举 workload |
| `checkpoint_root` | `checkpoint_root_node` | 定位 `.gz` / `.zstd` checkpoint |
| `cluster_config` | `cluster_config` | weighted stats 和 score 聚合 |
| `score_script` | `score_script` | 生成 `score.txt` |
| `artifact_name` | `artifact_name` | 性能 CI artifact 名称 |
| `comment` | `comment` | 性能任务说明 |

## 职责边界

共享 catalog 只描述 benchmark 数据，不表示每个消费者都支持其中的全部类型。

- `gem5-perf-template.yml` 可以使用完整 catalog，并继续负责 H-profile、SMT 环境、构建、
  执行、归档和算分。
- solver 复用路径，但继续在 `util/solver/types.py` 中维护自身能力限制。例如 catalog
  包含 `gcc12-spec06-smt-*` 和 `h-spec06-*`，solver 当前仍会明确拒绝这些类型。
- `manual-perf.yml` 和 `manual-solve.yml` 中的 `choice` 是 GitHub Actions 要求的静态 UI
  allowlist。`manual-solve.yml` 可以只列 solver 已支持的子集；这些列表不再包含切片路径。
- `custom_bin` 是 solver 的独立入口，不进入共享 catalog。

## 维护方法

### 修改已有切片路径

只修改 `util/xs_scripts/perf_benchmarks.py` 中对应的 `BenchmarkConfig`。性能模板和 solver
会在同一个 commit 中自动使用新值，不再修改 `util/solver/executor/benchmarks.py`。

本地检查 resolver：

```bash
python3 util/xs_scripts/perf_benchmarks.py gcc15-spec06-0.3c
```

不传 `--github-output` 时会打印 JSON，便于检查最终路径。性能 workflow 使用
`--github-output "$GITHUB_OUTPUT"` 生成兼容原模板的 step outputs。

### 新增 benchmark 类型

1. 在 `util/xs_scripts/perf_benchmarks.py` 增加一个 `BenchmarkConfig`。
2. 在需要暴露该类型的手动 workflow 中增加静态 `choice`；不支持该类型的入口不要增加。
3. 如果 solver 对该类型有额外能力限制，在 solver validation 中维护限制，不复制路径。
4. 运行下面的定向测试。

```bash
python3 util/xs_scripts/perf_benchmarks.py <benchmark-type>
python3 -m py_compile \
  util/xs_scripts/perf_benchmarks.py \
  util/solver/executor/benchmarks.py
```

提交前应确认性能模板调用共享 resolver、solver 不再包含 NFS 切片路径、GitHub output
schema 保持兼容，以及 solver 的 SMT/H-profile 限制没有因共享 catalog 而放宽。
