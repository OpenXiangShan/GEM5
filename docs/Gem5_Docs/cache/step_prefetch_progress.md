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
| STEP 论文通读 | 已完成 | 背景文档记录动机、Sec. III 机制、Table I 位级基线，以及单核/多核/多级/敏感性结果 |
| 论文对照审计 | 已完成 | 明确 Jaccard 为严格 `> 0.75`，并区分论文低存储基线与 GEM5 本地参数/时序选择 |
| 现有 SMS 审计 | 已完成 | 现有 PHT 是单项饱和计数器，不可直接承载多 footprint 相似度 |
| 实施合同 | 已完成 | `step_prefetch_implementation.md` 定义状态、参数、复杂度和验证口径 |
| 文档提交 | 待提交 | 初始三份文档已在 `0ff4520602` 提交；本次源代码对齐和验证结果将在实现提交后单独提交 |
| STEP 代码 | 已实现，待提交 | `step.hh/.cc`、参数、source attribution、独立 `stepPb`、raw-demand hook，以及 Queue 接受后才更新 `FT.issued` 的 staged completion 均在当前 worktree |
| 单元/小测试 | 已完成 | `step.test.cc` 14/14 通过，覆盖生命周期、maturity、多 history 收敛、AT victim 训练、TOE、`issued` 和隔离 |
| `gem5.opt` 构建 | 已完成 | `scons build/RISCV/gem5.opt --gold-linker -j1` 通过；生成有效 ELF |
| omnetpp 单点 | 已完成诊断 smoke | 当前修订的 10k/1M 基线与 STEP 均正常恢复、difftest 无 mismatch；按最新要求不把本地 A/B 作为 CI 门槛或性能结论 |
| 完整 CI 回归 | 运行中 | manual-perf run `33056028992`；被测 SHA 为 `4b4a6fdfbe23c77020a6392ec3452f307fc3677a`，状态以 Actions 页面为准 |
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
5. STEP 用独立 `stepPb` 承接候选，不复用 legacy `sms_pfFilter`；PB-to-Queued
   handoff 才提交共享去重、trace 和 `pfGenerated`，且仅在 PF-control、跨页检查和
   `Queued::insert()` 接受后置 `FT.issued`。STEP 固定要求 `use_pf_buffer=true`、
   `prefetch_train=false`，`step_pf_level` 的有效 target 为 1、2、3；其中 target 2
   在 L2 停止既有低准确率 L2-to-L3 offload。两项都必须通过生成的 `config.ini`
   和单点 trace 分别审计。
6. 当前修订的本地诊断 smoke 使用指定 `omnetpp/6881` 切片、
   `riscv64-nemu-notama-tvalref-so` 和 2-channel DRAMsim3；10k 与 1M 均以
   `max instruction count` 正常退出。它们仅用于确认恢复、配置和生命周期没有
   明显错误，完整性能结论以即将触发的 CI 归档为准。
7. 2026-08-27 已触发 `manual-perf.yml`：`configuration=kmhv3.py`、
   `benchmark_type=gcc15-spec06-1.0c`、`specific_benchmarks` 为空（该 workflow
   的 `checkpoint.lst` 全集）、`extra_args=--enable-step`、
   `distributed_servers=default`、`distributed_jobs_per_server=32`。workflow 将
   `default` 展开为 `node020-node034,node036-node039`；run URL 为
   `https://github.com/OpenXiangShan/GEM5/actions/runs/33056028992`。

## 已废弃的本地尝试

旧目录 `out/omnetpp-*-dramsim-notama-20260826` 发生在 FT 重复 offset 语义、PB
释放和统计口径修订之前，不参与当前结论。当前修订的诊断结果保存在
`out/omnetpp-step-final-{baseline,step}-{10k,1m}-20260827-v1`；由于用户最新
要求直接进行 CI，这些结果不替代完整 CI A/B。
