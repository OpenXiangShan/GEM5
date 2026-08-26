# Mockingjay L2 进展记录

## 2026-08-25：准备与设计

* 在 `/tmp/gem5-mockingjay-5361c12` 创建独立 worktree，分支为
  `codex/mockingjay-l2-5361c12`，基线精确为
  `5361c1248804755d285313f41dd73b7a299f7b48`。
* 保留原工作区及其无关的未跟踪文件。
* 阅读 `shah2022.pdf` 和作者公开的 ChampSim 参考实现。
* 确认集成点：`kmhv3.py` 中每个
  `system.l2_wrappers[i].slices[j].inner_cache` 配置独立的替换策略
  SimObject。
* 确认目标 checkpoint 存在：
  `/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260604/checkpoint/omnetpp/6881/_6881_0.962556_memory_.zstd`。
* 确认请求的 baseline run `32391965338` 在相同基线 SHA 上完成，且归档包含
  `performance-score-gcc15-spec06-1.0c/score.txt`。

## 2026-08-26：早期原型验证记录

以下结果属于早期“真实缓存旁路”原型；它们不能作为当前 `+INF_ETR` 插入
模型的验证结论，但 checkpoint、配置和 baseline 的环境信息仍可复用：

* 替换策略 GTest 在当时通过 10/10。
* cache timing GTest 在当时通过 5/5。
* `python3 -m py_compile configs/example/kmhv3.py` 和 `git diff --check`
  当时通过。
* 使用 checkpoint 兼容 reference 和本地 DDR4 fallback 的一百万指令 smoke 已
  进入真实仿真，`simInsts=1000007`，
  `system.cpu.committedInsts=1000007`。
* 生成的 `config.ini` 当时确认四个独立 L2 policy，均为 `MockingjayL2RP`，
  `num_sets=1024`、`num_ways=8`、`block_bits=6`、`slice_bits=2`、
  `sampled_sets=8`、`sampled_tag_bits=12`、`rdp_entries=512`；每个 slice 都有
  采样历史、RDP、提升、插入和 aging 活动。

本次工作树修订按审查要求删除真实缓存旁路及其 BaseCache/tags/
packet-aware 接口改动，改为所有填充正常分配；在更新采样历史前，若预测为
scan 或预测 ETR 大于所选牺牲行绝对 ETR，则以 `+INF_ETR` 插入。下节记录该
修订已完成的本地验证。

## 2026-08-26：当前修订验证

* 在 `beeb9ff4b80262a45712d7c20b7829b476ad3c03` 上串行重建 `gem5.opt` 和替换
  策略 GTest；17/17 个策略测试通过，其中新增软件预取不训练回归。
* `python3 -m py_compile configs/example/kmhv3.py` 和 `git diff --check` 通过。
* checkpoint 冒烟测试在 `/tmp/mockingjay-l2-omnetpp-6881-review-beeb9ff4b8` 完成，
  `simInsts=1000008`、`system.cpu.committedInsts=1000008`。
* `config.ini` 确认四个独立 `MockingjayL2RP`，默认 slice geometry 为
  `1024 sets × 8 ways`，`block_bits=6`、`slice_bits=2`、`sampled_sets=8`、
  `sampled_tag_bits=12`、`rdp_entries=512`；四个 `maxEtrInsertions` 为
  13、4、0、2。
* 冒烟测试输出没有 `bypasses` 或 `policy_bypassed`。本地 DDR4 fallback 只作
  功能/配置证据，不作 CI DRAMsim3 性能证据。

本次修订的冒烟测试使用命令：

```bash
GCBV_REF_SO=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-notama-tvalref-so \
./build/RISCV/gem5.opt \
  -d /tmp/mockingjay-l2-omnetpp-6881-review-beeb9ff4b8 \
  ./configs/example/kmhv3.py --mem-type=DDR4_2400_8x8 -I 1000000 \
  --generic-rv-cpt=/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260604/checkpoint/omnetpp/6881/_6881_0.962556_memory_.zstd
```

默认 interpreter reference (`riscv64-nemu-interpreter-so`) 会在 checkpoint 初始
`mstateen0` CSR 指令（`sn:163`）失败，因此冒烟测试使用
`riscv64-nemu-notama-tvalref-so`。本地 DDR4 时序不能与 CI 的 DRAMsim3 做性能
比较。

## 工作树修订状态

历史代码的审查顺序是：

1. `96ceca6e3b`：早期旁路原型的 bookkeeping 修复；本次工作树修订会移除其
   相关缓存流水线代码。
2. `ee4aedf618`：Mockingjay 替换策略、配置与初始文档。
3. `c2dbe9837b`：早期 admitted-fill 回归；在本次模型中由替换策略的
   `+INF_ETR` 插入测试替代。
4. 本次工作树修订：删除旁路 plumbing、改成 `+INF_ETR`、中文文档和重新
   验证；以分支最新提交作为审查检查点。

本次代码审查不触发新的远程性能 CI；下节记录已存在的一次完整候选归档，以及它
不能用于性能归因的原因。

## 候选 CI 与受控 A/B 合同

* Baseline run：`32391965338`；有效 job：`96499960567`；基线 SHA：
  `5361c1248804755d285313f41dd73b7a299f7b48`。
* Baseline 归档：
  `/nfs/home/share/gem5_ci/performance_data/gcc15-spec06-1.0c/20260821_003117_5361c12488_kmhv3_run102`；
  score 为 `20.612401866596542`。
* 候选 workflow：`32941495780`（run 968）；提交为
  `35f340a2e3a989fb3d2ea8c1ea4d751a4ff618f4`，归档为
  `/nfs/home/share/gem5_ci/performance_data/gcc15-spec06-1.0c/20260826_151522_35f340a2e3_kmhv3_run968`。
  使用 `kmhv3.py` 和 `gcc15-spec06-1.0c`，1112 个 workload 全部完成，1112 份
  `config.ini` 和 `stats.txt` 完整，无 abort。每份配置均含四个
  `MockingjayL2RP`，故可确认策略被实际实例化。
* 该候选相对基线的总分/GHz 为 `20.546875856038877`，观察变化为 `-0.317896%`；
  整数分变化 `+0.192332%`，浮点分变化 `-0.676492%`。此记录仅描述归档结果，
  不是性能归因。
* **候选不满足受控 A/B 合同。** 基线的 `distributed_servers` 为空，走本地
  parallel path；候选输入为 `default`，展开为共享节点池并走 `distributed_sim.py`。
  执行路径变化，所以不能把任何 score 或 stats 差异归因于 Mockingjay，不能将其
  用于 solver/DSE，也不能用它替代重跑。
* 待运行任务必须使用本分支完整 40 位 SHA、`kmhv3.py`、
  `gcc15-spec06-1.0c`、完整整数 slice 集合、空 `distributed_servers`（CI
  parallel path）和 CI DRAMsim3。
* 在性能结论之前，必须归档并核对 `config.ini`、`score.txt` 和 manifest；没有
  这些产物不能将 setup 成功或参数请求视为性能证据。
