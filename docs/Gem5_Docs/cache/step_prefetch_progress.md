# STEP 预取器实施进度

## 实验身份

- 基线提交：`5361c1248804755d285313f41dd73b7a299f7b48`
- 分支：`codex/step-prefetch-5361c12`
- 隔离 worktree：`/tmp/gem5-step-prefetch-20260826`
- 目标配置：`configs/example/kmhv3.py`
- 单点 checkpoint：
  `/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260604/checkpoint/omnetpp/6881/_6881_0.962556_memory_.zstd`
- 完整回归目标：`gcc15-spec06-1.0c` 的 int checkpoint 集合
- 外部 baseline：GitHub Actions run `32391965338`

## 进度表

| 阶段 | 状态 | 证据或下一步 |
| --- | --- | --- |
| 隔离基线与分支 | 已完成 | worktree HEAD 为精确 `5361c124...`，主工作区未修改 |
| STEP 论文通读 | 已完成 | 背景文档记录 Sec. I--V、Table I 和关键实验结果 |
| 现有 SMS 审计 | 已完成 | 现有 PHT 是单项饱和计数器，不可直接承载多 footprint 相似度 |
| 实施合同 | 已完成 | `step_prefetch_implementation.md` 定义状态、参数、复杂度和验证口径 |
| 文档提交 | 进行中 | 写入后检查格式并作为独立 commit |
| STEP 代码 | 未开始 | 下一提交只引入可关闭的 STEP FT/AT/PHT 和统计 |
| 单元/小测试 | 未开始 | 覆盖事件、置信度、maturity、训练和关闭开关 |
| `gem5.opt` 构建 | 未开始 | 构建后记录命令和 SHA |
| omnetpp 单点 | 未开始 | 先短窗口，再完整同长度 A/B；审计 config.ini 和 stats |
| 完整 CI 回归 | 未开始 | 本地验证、提交、推送后回显 `manual-perf` 命令待确认 |
| STEP 参数 DSE | 未开始 | 仅在完整 CI 正收益后定义代表切片、目标和预算 |

## 已做事实核对

1. `manual-perf.yml` 支持 `configuration=kmhv3.py` 与
   `benchmark_type=gcc15-spec06-1.0c`。
2. 该集合的 checkpoint 根为
   `/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260604/checkpoint`，
   与指定 omnetpp 切片一致。
3. 当前主工作区含用户未跟踪文件，因此所有本任务写入都只在上述隔离 worktree 进行。
4. 远端 CI、push 和 solver dispatch 都是可见的外部状态变更：构建、单点、文档和
   代码提交完成后，将先给出精确命令/输入和已验证的本地证据，再执行需要确认的触发。
