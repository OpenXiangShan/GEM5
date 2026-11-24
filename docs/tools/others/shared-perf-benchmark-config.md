# 性能 CI 与 Solver 的切片与执行

Perf 和 Solver 从 `util/xs_scripts/ci/benchmarks.py` 读取同一份 profile。SPEC 的权重 JSON 决定选点，每个 `workload/point` 在切片根目录下对应一个镜像。Trace 从六列清单读取文件和测量区间。所有作业经过筛选后交给 `runner.py`，可以在本机执行，也可以用 SSH 分配到共享池。空服务器列表表示本机，`default` 选择共同的节点池。Perf 在启动时探测节点，池内节点全部失联时再重探。Solver 在排队任务缺少空槽时按间隔重新探测，继续控制 trial、overlay、超时和目标函数。

GCC12 weekly 单线程及 SMT 1c 共用切片目录 `spec06_cpts/gcc12-spec06-paired.json`，包含 1,167 个成对镜像。它保留旧权重和指令数，移除了 SMT 缺镜像的两个 HMMER 第 0 点。手动 SMT INT 1c 使用同目录下的 682 点 INT JSON。SMT 0.3c 继续使用 `spec06_0.3c.json` 的 148 点。

Weekly 的 XSCC SPEC06 和双 hart GCC16 SPEC17 分别使用 `xscc-spec06-1.0c` 和 `gcc16-spec17-smt-1.0c`。XSCC 使用 32 GB 内存及对应的单核 dedup REF，双 hart 使用 SMT 模式的 16 GB 内存及 multi REF。两者沿用原先的 `custom/<profile目录名>` 归档，历史结果仍在同一目录。

新增 SPEC profile 时，在 catalog 登记根目录、权重文件和 SPEC 版本。需要出现在手动菜单时再增加对应 choice。Perf 使用被测 checkout 的配置、PGO 构建和 REF lock，调用 workflow 版本的 CI 工具。一次运行写入独立归档，`checkpoints.lst` 记录实际选点，`cluster.json` 保存计分权重。每个切片的 `log.txt` 记录命令和主机，终态由 launcher 统一写入。摘要最多列出 20 个未完成点，完整清单写入归档的 `metadata.txt`。

切片退出后立即释放执行槽位，`completed` 表示进程正常退出。算分机在整批结束后读取各点的 stats 尾标，对仍不可读的点共用 60 秒重试窗口。到期后按执行机批量补查，摘要和 `metadata.txt` 分别记录算分机与执行机的读取结果。严格模式下缺点会返回失败并跳过出分；显式允许部分结果的 SPEC 任务会标注可计分点数。

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
python3 -m util.xs_scripts.ci.run_perf_ci --dry-run --benchmark-type custom \
  --checkpoint-path /nfs/home/share/checkpoints_profiles/spec17_rate_gcc16_rva23_novec_260904 \
  --config-path configs/example/kmhv3.py
```

入口在派发前核对输入；缺镜像或歧义会直接报错。归档目录使用
`custom/<profile目录名>/<时间_SHA_配置_run编号>`，内建类型仍使用原路径。
路径与 JSON 内容摘要保留在 metadata 的 `benchmark_type` 中，用于区分同名
profile 或权重更新；目录名相同不代表结果可以直接作为 baseline 比较。
归档保存实际运行列表 `checkpoints.lst`、权重 `cluster.json` 和源路径 metadata；
同一 SPEC 类型的不同 profile 仍需显式匹配后再做性能比较。
此入口只扩展手动性能 CI，不扩展 solver 的可选类型。
