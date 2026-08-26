# Mockingjay L2 审查交接

## 当前检查点

* 分支：`codex/mockingjay-l2-5361c12`
* 基线：`5361c1248804755d285313f41dd73b7a299f7b48`
* 初始已发布检查点：`c95ff7ac13c9e21dc505a266f6ea460f3f422ae3`
* 候选 CI 所用检查点：`35f340a2e3a989fb3d2ea8c1ea4d751a4ff618f4`。
* 当前源码验证检查点：`beeb9ff4b80262a45712d7c20b7829b476ad3c03`。
* 临时审查分支：`codex/mockingjay-l2-review-summary-20260826`；该分支保留同一套
  实现，并额外包含软件预取不训练的修正、中文审查快照和本文档同步。
* 当前审查范围：在上述检查点之上移除真实缓存旁路，并改为
  `+INF_ETR` 正常插入；所有新增 Markdown 文档已改用中文。
* CI 状态：已有一次完整候选 CI 归档，详见下方“验证状态”；其执行路径与基线
  不一致，不能作为受控性能 A/B 结论。

初始实现包含三个主要代码提交：旁路记账 `96ceca6e3b`、替换策略/配置集成
`ee4aedf618`、已接纳填充回归 `c2dbe9837b`。当前修订的目标是收敛其范围：保留
每个 slice 的 `MockingjayL2RP`、采样历史、RDP、ETR aging 和牺牲行选择；删除
`BaseCache`、tags、VIPT tags、Dueling replacement policy 和 cache timing
test 中为直接旁路增加的接口与路径。

低复用预测不再使 `getVictim` 返回空指针。每次填充仍正常选择牺牲行、分配
缓存行、执行一致性、响应和回填通知；在策略的
`reset(data, pkt)` 中，在更新采样历史前预测为 scan 或预测 ETR 大于所选牺牲行
绝对 ETR 的可训练填充（包括硬件预取）被赋值 `+INF_ETR`，使其
在随后的标准牺牲行选择中优先于绝对 ETR 更小的缓存行被踢出。绝对值相同的
负 ETR 仍优先，因而 `-INF_ETR` writeback 会先于 `+INF_ETR` 缓存行被选中。该
取舍保留淘汰顺序的性能因果链，而不改变缓存的功能语义和时序流水线。

## 主要文件

* `src/mem/cache/replacement_policies/mockingjay_l2_rp.{hh,cc}`：策略状态、
  RDP/采样训练、ETR aging、普通牺牲行选择和 `+INF_ETR` 插入。
* `src/mem/cache/replacement_policies/mockingjay_l2_rp.test.cc`：几何参数、
  采样、RDP、aging、平局、无 PC、软件预取隔离和 scan 的 `+INF_ETR` 插入测试。
* `configs/example/kmhv3.py`：根据每个 L2 slice 的几何参数构造独立 policy。
* `docs/Gem5_Docs/cache/mockingjay_background.md`：论文机制与迁移边界。
* `docs/Gem5_Docs/cache/mockingjay_l2_implementation.md`：建模合同、参数、
  算法和精度边界。
* `docs/Gem5_Docs/cache/mockingjay_l2_progress.md`：历史验证、当前验证缺口和
  冻结的 CI A/B 合同。

## 审查重点

1. 每个 L2 slice 是否拥有独立的预测器状态，且 `kmhv3.py` 的 geometry
   推导与实际 `inner_cache` 一致。
2. 当前差异是否已完全去除按 packet 感知的牺牲行接口、`policy_bypassed`、
   专用直接响应旁路和专门的 cache timing test，从而使 cache/tags 接口回到基线。
3. 论文旁路对应条件是否只影响插入 ETR：填充必须仍调用正常的 `reset`，先用
   训练前预测完成判定、再训练一次采样历史，并以 `+INF_ETR` 优先于绝对 ETR
   更小的缓存行；同绝对值负 ETR 的 tie-break 必须保持不变。
4. RDP signature 是否取 CRC hash 低位，以及有符号 ETR 的绝对值/负值优先
   tie-break 是否与实现约定一致。
5. 构造函数是否拒绝跨越 `Addr` 位宽的地址字段、溢出的采样 bucket 几何，以及
   小于或等于历史窗口的时间戳模数。

## 验证状态

初始旁路原型曾通过 policy GTest 10/10、cache timing GTest 5/5；这些结果只
作为历史参考，不能覆盖当前 `+INF_ETR` 语义。本次修订已完成以下验证：

* 在 `beeb9ff4b8` 上串行重建替换策略 GTest 和 `build/RISCV/gem5.opt`。
* `mockingjay_l2_rp.test.opt` 的 17/17 测试通过，覆盖训练前判定、最大 ETR
  替换顺序、硬件预取、软件预取隔离、其他非训练流量和非法几何参数。
* `python3 -m py_compile configs/example/kmhv3.py` 通过，`git diff --check`
  通过。
* 使用 checkpoint 兼容 reference 和本地 DDR4 fallback 的一百万指令冒烟测试
  在 `/tmp/mockingjay-l2-omnetpp-6881-review-beeb9ff4b8` 完成：
  `simInsts=1000008`、`system.cpu.committedInsts=1000008`。
* `config.ini` 确认四个独立 `MockingjayL2RP`，参数为
  `num_sets=1024`、`num_ways=8`、`block_bits=6`、`slice_bits=2`、
  `sampled_sets=8`、`sampled_tag_bits=12`、`rdp_entries=512`；四个
  `maxEtrInsertions` 分别为 13、4、0、2。
* 输出中没有 `bypasses` 或 `policy_bypassed`；该冒烟测试只证明功能和配置，
  不代表匹配 CI DRAMsim3 的性能结果。

候选 CI `32941495780`（run 968）使用 `35f340a2e3`、`kmhv3.py` 和
`gcc15-spec06-1.0c`，归档位于
`/nfs/home/share/gem5_ci/performance_data/gcc15-spec06-1.0c/20260826_151522_35f340a2e3_kmhv3_run968`。
1112 个 workload 全部完成，包含 1112 份 `config.ini` 和 `stats.txt`，无 abort；
每份配置都实例化四个 `MockingjayL2RP`，并有非零的最大 ETR 插入统计，故可确认
策略实际生效。归档中没有 `policy_bypassed` 或 `bypasses`。

相对基线归档的观察值如下（完整候选包含 Int+FP，因此这里只把 Int 行作为后续
整数 A/B 的比较口径）：

| 指标 | 基线 | 候选 | 变化 |
| --- | ---: | ---: | ---: |
| 总分/GHz | 20.6124018666 | 20.5468758560 | -0.317896% |
| 整数分/GHz | 18.6756638615 | 18.7115831569 | +0.192332% |
| 浮点分/GHz | 22.0992462676 | 21.9497466541 | -0.676492% |

**这不是受控 A/B。** 基线归档使用空 `distributed_servers` 的本地 parallel
path；候选使用 `default` 展开的共享节点池和 `distributed_sim.py` 路径。执行路径
不同，因此上表只能记录“完整候选运行完成后的观察值”，不能归因于 Mockingjay，
也不能用于 solver 或 DSE 决策。当前审阅分支的软件预取隔离修正也不在该候选 CI
的源码中。要形成性能结论，必须用同一完整 SHA、`kmhv3.py`、
`gcc15-spec06-1.0c`、`specific_benchmarks=perlbench,bzip2,gcc,mcf,gobmk,hmmer,sjeng,libquantum,h264ref,omnetpp,astar,xalancbmk`
（1112 中的 697 个整数 slice）、空 `distributed_servers` 和 CI DRAMsim3 重跑
成对实验，并审计归档的 `config.ini`、`score.txt` 与 manifest。

## 已知边界

* 这是行为级替换模型，不是 RTL 的逐拍实现。
* 不实现论文的真实缓存旁路；低复用填充会短暂占用一个 way，直到正常
  牺牲行选择淘汰它。
* 本地冒烟测试使用 checkpoint 兼容 reference 和 DDR4 fallback，不能代替匹配
  CI DRAMsim3 的性能 A/B。
