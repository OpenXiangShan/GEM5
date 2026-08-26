# Mockingjay L2 审查快照

## 快照目的

这是供代码审查使用的临时分支快照。它与实现分支采用同一套实现，并额外保留
软件预取训练隔离、中文审查材料和验证记录；本分支没有触发新的远程性能 CI。

| 项目 | 值 |
| --- | --- |
| 临时审查分支 | `codex/mockingjay-l2-review-summary-20260826` |
| 实现分支 | `codex/mockingjay-l2-5361c12` |
| 候选 CI 所用检查点 | `35f340a2e3` |
| 当前源码验证检查点 | `beeb9ff4b8`：软件预取不训练、slice 容量报错文案和中文记录同步 |
| 基线 | `5361c1248804755d285313f41dd73b7a299f7b48` |
| 性能 CI | run 968 已完成，但不是受控 A/B |

当前树实现的是每个对齐 L2 slice 独立的 Mockingjay 行为级替换策略。用户指定的
收敛原则已经落实：新增 Markdown 使用中文；不实现论文的真实缓存旁路，而是
让对应填充正常进入缓存，并把该缓存行标记为最大正 ETR，以便在后续正常
替换中尽快被淘汰。

## 范围与代码摘要

### 配置与对象归属

`configs/example/kmhv3.py` 只在非 `classic_l2` 的对齐 L2 路径中，为每个
`l2_wrapper.slices[j].inner_cache` 构造一个新的 `MockingjayL2RP`。每个实例
拥有自己的采样 cache、RDP、per-set clock、时间戳和 ETR 状态，因此预测器不会
跨 slice 共享。

配置根据实际 `inner_cache` 的 size、associativity、cache line 大小和 slice 数
推导 `num_sets`、`block_bits`、`slice_bits`、`sampled_sets`、
`sampled_tag_bits` 与 `rdp_entries`。几何检查拒绝非二次幂布局，并将 slice
容量限制为 8 KiB 到 2 GiB，避免采样地址字段和预测器规模失真。

新增的 SimObject 注册位于
`src/mem/cache/replacement_policies/ReplacementPolicies.py` 和同目录的
`SConscript`；模型本体位于 `mockingjay_l2_rp.{hh,cc}`，测试位于
`mockingjay_l2_rp.test.cc`。

### 低复用填充的处理

所有 fill 保留 GEM5 原有分配链：

```text
findVictim -> handleEvictions -> insertBlock -> replacementPolicy.reset
```

`MockingjayL2::getVictim()` 始终返回一个候选缓存行，不会以空指针表达旁路。
`MockingjayL2::reset(data, pkt)` 在更新本次采样历史之前读取 RDP 预测；若预测为
scan，或预测 ETR 大于本次牺牲行的绝对 ETR，则仍正常插入该缓存行，但把它设为
`+INF_ETR` 并递增 `maxEtrInsertions`。后续 `selectVictim()` 按 `abs(ETR)` 最大
选择牺牲行，因此该缓存行相比绝对 ETR 更小的缓存行会更早成为替换候选。

这不是即时旁路：缓存行仍会短暂占用一个 way，且仍经历原有一致性、响应、回填
和 MSHR 流程。其目的只是用普通替换顺序近似论文中低复用填充的快速离开。
同绝对值时保留负 ETR 优先的规则，因此 `-INF_ETR` 的 writeback 会先于
`+INF_ETR` 缓存行被选中。

### 学习与替换规则

* RDP 使用 PC、hit/miss 与预取标志的 CRC hash 低位索引；复用距离按同一物理
  L2 slice 的同一 set 的访问次数统计。
* 可训练流量包括普通请求和硬件预取。软件预取、writeback、eviction、
  `WriteClean` 与 cache-maintenance 流量不更新采样历史或 RDP；软件预取会在
  cache 内部丢失 PC，因此必须排除，避免污染 no-PC 表项。writeback fill 以
  `-INF_ETR` 插入。
* 派生阈值为 `INF_RD = num_ways * history_multiplier - 1`、
  `MAX_RD = INF_RD - scan_threshold_margin`、
  `INF_ETR = num_ways * history_multiplier / aging_granularity - 1`。
* victim 选择优先 invalid way；否则选择 `abs(ETR)` 最大的有效 line，绝对值
  相同则负 ETR 优先。
* 正/负 ETR victim 与 invalid victim 统计记录的是 `getVictim()` 已选候选，
  在 cache 真正处理淘汰前递增；分配失败时也可能出现，因此不可直接解释为实际
  淘汰数。

## 与早期原型的差异

早期历史提交曾试验真实旁路。最终检查点已将这类接口和路径撤回：

* `BaseCache`、tags、VIPT tags、Dueling replacement policy 均回到基线接口；
* 不存在 `policy_bypassed`、专用直接响应旁路或按 packet 感知的牺牲行 API；
* 不再保留为旁路增加的 cache timing test；对应行为由策略级
  `+INF_ETR` 测试覆盖。

因此本次差异的功能边界是 replacement policy 和配置，不改变 cache 的功能
语义或时序流水线。

## 已完成验证

* `build/RISCV/mem/cache/replacement_policies/mockingjay_l2_rp.test.opt`：
  17/17 通过，覆盖采样训练、scan、训练前决策、最大 ETR 插入、硬件预取、
  软件预取隔离、其他非训练流量、per-set 隔离、平局和非法几何参数。
* `build/RISCV/gem5.opt` 已在 `beeb9ff4b8` 上针对当前策略重新编译链接。
* `python3 -m py_compile configs/example/kmhv3.py` 和 `git diff --check` 通过。
* checkpoint 冒烟使用 `omnetpp/6881`，输出目录为
  `/tmp/mockingjay-l2-omnetpp-6881-review-beeb9ff4b8`；完成
  `simInsts=1000008` 和 `system.cpu.committedInsts=1000008`。生成的
  `config.ini` 确认四个独立 `MockingjayL2RP`，四个 slice 的
  `maxEtrInsertions` 为 13、4、0、2。

冒烟使用本地 DDR4 fallback 与 checkpoint 兼容 reference，只能证明构造、配置
和基本执行正确，不能替代 CI DRAMsim3 下的性能结论。

候选 CI `32941495780`（run 968）已在 `35f340a2e3` 上完成。归档包含 1112 个
completed workload、1112 份 `config.ini`、1112 份 `stats.txt`，无 abort；每份配置
都有四个 `MockingjayL2RP`，且 `maxEtrInsertions` 非零。相对基线，总分/GHz 为
20.6124018666 到 20.5468758560（-0.317896%）。但候选使用 `default` 的共享节点
池和 `distributed_sim.py`，基线使用空 `distributed_servers` 的本地 parallel path，
因此这只是实现生效的非受控观察，不是 Mockingjay 的性能归因，也不可用于 solver
或 DSE。当前软件预取修正不在该候选 CI 中。

## 建议审查点

1. 检查每个 `inner_cache` 是否获得独立 `MockingjayL2RP`，以及参数推导是否与
   实际 slice geometry 一致。
2. 检查 `getVictim()` 总是返回实际候选 line，且 `reset()` 的最大 ETR 决定只
   改变 insertion priority，不改变正常 fill 链。
3. 检查低复用决策使用训练前 RDP 结果，而训练本身仍只发生一次；检查正负 ETR
   的绝对值排序与负值平局规则。
4. 检查软件预取及其他非训练流量不会污染 demand/硬件预取学习历史，硬件预取
   fill 则仍可训练。
5. 检查构造函数的地址、时间戳和容量防护是否满足预期的参数空间。

可从以下命令开始：

```bash
git diff --stat 5361c1248804755d285313f41dd73b7a299f7b48 HEAD
git diff 5361c1248804755d285313f41dd73b7a299f7b48 HEAD -- \
  configs/example/kmhv3.py \
  src/mem/cache/replacement_policies/ReplacementPolicies.py \
  src/mem/cache/replacement_policies/mockingjay_l2_rp.cc \
  src/mem/cache/replacement_policies/mockingjay_l2_rp.hh \
  src/mem/cache/replacement_policies/mockingjay_l2_rp.test.cc
```

## 尚未完成的事项

尚未完成受控性能 A/B。后续性能验证必须固定完整 SHA、`kmhv3.py`、
`gcc15-spec06-1.0c`、完整整数 slice 集合、CI 的 parallel path 和 DRAMsim3，
并同时审计归档的 `config.ini`、`score.txt` 与 manifest，之后才能讨论收益或
进入 solver 参数探索。
