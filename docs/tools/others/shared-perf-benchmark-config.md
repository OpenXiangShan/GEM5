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
| `comment` | `comment` | 性能任务说明 |

## 职责边界

共享 catalog 只描述 benchmark 数据，不表示每个消费者都支持其中的全部类型。

- `gem5-perf-template.yml` 可以使用完整 catalog，并继续负责 H-profile、SMT 环境、构建、
  执行、归档、算分和基于 config 类型生成 artifact 名称。
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
python3 util/xs_scripts/perf_benchmarks.py spec06-rva23-novec-gcc16-0.3c
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

## 手动运行新的切片 profile

在 `manual-perf.yml` 的 Run workflow 中填写：

- `benchmark_type`: `custom`，从输入路径名中的 `spec06`、`spec17`、`spec26`
  识别计分类型；路径没有这些标记或包含多个类型时，显式选择
  `custom-spec06`、`custom-spec17`、`custom-spec26`。
- `checkpoint_path`: profile 根目录，或直接填写 `checkpoint` / `checkpoint-0-0-0`
  目录。runner 必须能读取该路径；分布式运行时所有节点都必须可见。
- `json_path`: 通常留空。自动查找切片目录内的 `checkpoints_all.json`、
  `cluster-0-0.json`，以及上一级的 `json/checkpoints_all.json`、
  `cluster-0-0.json`。缺失或存在多个候选时，必须明确填写对应权重 JSON 的绝对路径。

例如 `checkpoint_path` 填写：

```text
/nfs/home/share/checkpoints_profiles/spec17_rate_gcc16_rva23_novec_260904
```

选择 `custom` 即可自动使用 SPEC17 计分脚本。这里的默认 **1c 是完整切片覆盖率**，
与运行单核还是 SMT 无关；程序按 JSON 的全部 `workload/point` 生成列表，
支持每个点目录下唯一一个 `.gz` 或 `.zstd` 镜像，不依赖旧 CI 的列表或权重。
若显式指定部分覆盖 JSON，运行范围也随之缩小；`specific_benchmarks` 仍可进一步筛选。
JSON 必须包含每个 workload 的 `insts` 和 `points` 权重。

NEMU 沿用现有统一 REF 选择逻辑。单核使用普通 configuration；双 hart 切片需显式
选择 `smt_idealkmhv3.py`，不能仅凭 SPEC 类型推断核数。

本地只检查目录和配置，不启动 GEM5：

```bash
python3 util/xs_scripts/perf_benchmarks.py custom \
  --checkpoint-path /nfs/home/share/checkpoints_profiles/spec17_rate_gcc16_rva23_novec_260904 \
  --output-dir /tmp/gem5-custom-perf
```

预检在 CI 构建前执行；缺镜像或歧义会直接报错。归档目录使用
`custom-specXX-<路径与JSON内容摘要>`，避免混入内建 profile 的 baseline。
归档保存实际运行列表 `checkpoints.lst`、权重 `cluster.json` 和源路径 metadata；
同一 SPEC 类型的不同 profile 仍需显式匹配后再做性能比较。
此入口只扩展手动性能 CI，不扩展 solver 的可选类型。
