# STEP 在 KMHv3 GEM5 中的实施方案

## 范围和建模合同

本实现把 STEP 作为 L1 `XSCompositePrefetcher` 中 SMS PHT 的可选替代
路径。`enable_step=False` 保留现有 `sms.cc` 行为；`enable_step=True`
保留复合预取器的 stream/stride、训练入口、去重和发射通道，但用 STEP
产生空间 footprint 候选。这使 A/B 只改变空间 footprint 的决策机制，
不把 Berti、XS stream 或 L2/L3 转发变化混入归因。

性能因果链为：

```text
每个合格的 demand load（非预取、非写、非 StorePFtrain，且具有 PC/所需地址）
  -> STEP FT/AT/PHT 状态与 FOE/SOE/TOE 决策
  -> STEP PB 预检、有限容量缓冲与 PB-to-Queued 去重提交
  -> L1 请求或 L2/L3 hint、MSHR/带宽竞争与需求 miss
  -> IPC、prefetch useful/unused、STEP 分阶段统计
```

模型保留的细粒度行为是 region 内前三次访问的 cache-line offset 顺序、
PHT 容量/组相联替换、FOE/SOE/TOE 的发射或推迟、以及候选进入既有队列
后的背压。它不复刻 ChampSim 的全部 PB 微时序，但以独立的 `stepPb` 调用既有
`PrefetchFilter` 实现固定容量的 PB；它与 legacy SMS 的 `sms_pfFilter` 不共享
状态。STEP 只支持 `use_pf_buffer=True`：候选先通过共享 block/page 过滤预检，
再进入 `stepPb`，由 `Queued` 沿用既有队列、优先级和跨级发射语义，避免为同一
候选建立两套背压模型。

论文 PB 仅在 PQ 已满时接住请求，并等待未来 access trigger 重试。当前模型的
PB 是 cycle-driven staging：每个通过预检的候选都先写入 `stepPb`，下一个可用
cycle 由 `Queued::PFSendEventWrapper()` 尝试交接。PB 选中 line 后才提交共享
去重、trace 和 `pfGenerated`；其 `FT.issued` 则继续延后，只有该 line 通过
PF-control、跨页检查和 `Queued::insert()` 后才置位。若交接被拒，FT 保持可发射，
后续 TOE 仍能尝试更精确的 footprint。PB replacement 或交接拒绝仍不会恢复已被
消费的 PB line，因此该模型保留有限缓冲和队列竞争，但没有论文 future-access
retry 的完整语义。

论文的 FT/AT 使用 hashed page number，PHT 用 FO 选择组并以 SO/TO 作 tag，
且 Table I 将每项 3-bit LRU 状态计入 10.50 KB。GEM5 为了保持 ContextID 和
secure 隔离，以完整 region/context payload 比较，并用 `AssociativeSet` 的
index/replacement 和单调 `sequence` 表示候选新旧；这保留了容量和近期 history
语义，却不复刻论文的 hash 冲突分布、LRU 位级实现或面积。论文也没有规定
page/PC hash 函数，故这些选择都属于本地模型，而非论文参数（PDF p.5--6，Sec. III-D/F，Table I）。

## 当前 SMS 与 STEP 的接口差异

`src/mem/cache/prefetch/sms.cc` 当前路径的 `ACTEntry` 在 region 访问中
累积 `regionBits`，并在 `updatePht()` 中写入由 PC/context 索引的
`PhtEntry::hist`。`hist` 是相对首 offset 的饱和计数器向量，`phtLookup()`
只命中一条聚合项。它适合当前 SMS 置信度语义，却没有多条独立 footprint
或最近顺序，不能实现论文的候选相似度。

STEP 因此使用独立的、固定容量的状态，并只复用下列现有公共能力：

| 现有能力 | STEP 的使用方式 |
| --- | --- |
| `Base::observeRawDemandAccess()` | 在 legacy miss/prefetch-first-hit 策略之前，驱动 STEP 对每个合格 demand load 的观察和学习 |
| `contextKey()`、`pfi.contextId()`、secure bit | 对 FT/AT/PHT 状态保持上下文隔离；STEP 私有表另以完整 payload 比较 InvalidContextID、ContextID 和 secure 域 |
| `AssociativeSet`、索引与替换策略 | 实现固定容量、参数化的 FT/AT/PHT；PB 在 set lookup 后做完整 region/context/secure 比较 |
| `sendPFWithFilter()` | 复用 block/page 去重、ArchDB trace、priority 与 L2/L3 hint 标记 |
| `stepPb` / `Queued` | STEP 使用独立的 32-entry `PrefetchFilter` 作为 PB；PB 到 Queued 的交接才提交共享去重、trace 和 `pfGenerated`。它不与 `sms_pfFilter` 共享状态 |

## 新状态和转移

### STEP FT

`StepFilterEntry` 以 4 KB region 地址和 ContextID 为键，保存 `firstOffset`、
`secondOffset`、`pcHash`、`issued`。它只跟踪还未进入 STEP AT 的 region。

- 首次访问：记录 FO，做 FOE lookup。
- 第二次访问：记录 SO；若尚未发射且 `step_enable_soe=True`，
  做 SOE lookup。
- 第三次访问：做 TOE lookup；随后分配 STEP AT，初始化 footprint
  为前三个 offset 对应 bit 的并集，并使 FT 项失效。三个 offset 可以相同；FT
  的门槛是访问次数，严格对应论文的 “fewer than three accesses”。

### STEP AT

`StepActiveEntry` 以 region 地址和 ContextID 为键，保存前三个 offset、
12-bit PC hash 和 64-bit footprint。后续访问对 footprint 对应 bit 置位。AT victim
驱逐时训练 PHT；少于三次访问的 region 不写入 PHT。这等价于论文“第三次访问后
进入 AT”的学习门槛，且不把同一 cache line 的重复访问错误地当作无效事件。

### STEP PHT

`StepPatternEntry` 以 FO 选择组，保存 FO/SO/TO、完整 64-bit footprint、
PC hash、maturity、ContextID 和单调 `sequence`。因为同一个 FO 要保留多条
原始历史，lookup 仅扫描该 set 的候选 ways；默认 8-way，因此每个访问的
复杂度是 `O(step_pht_assoc)`，而非全表扫描。

候选选择按最新 `sequence` 取最多 `step_confidence_entries` 条。FOE 需要
FO+PC hash，SOE 需要 FO+SO，TOE 需要 FO+SO+TO。每次 AT eviction 都写入
一个 PHT history position，不按完整 FO/SO/TO 原地合并，使同一事件的多个近期
footprint 可以参与置信度计算；这是为 GEM5 的 recent-history 选择作的具体实现
选择。插入优先占用 invalid way，满时由现有替换策略选择 victim；若被替换项来自
同一 secure 域、ContextID、FO 且 PC hash 相同，新项标为 mature。该比较严格对应
论文的“同一 history position 被相同 PC 再次替换”的冷启动近似，不跨安全域或
上下文传播成熟度。

### 决策和发射

- FOE/SOE：候选为空则不发射；单项 FOE 还必须成熟；多项使用严格大于
  `step_confidence_threshold` 的 Jaccard 阈值，成功时对 footprint 求交集。
- TOE：选最新精确匹配项，发射其完整 footprint。
- 发射前移除当前已经观察到的 offset，剩余候选逐 line 调用
  `sendPFWithFilter(..., commit_filter=false)` 做共享 block/page 过滤预检，保留
  既有去重和目标 level 语义。`step_pf_level=1` 产生本层 L1 请求；`2`、`3`
  分别通过既有 `pfahead` 通道指向 L2、L3。PB 仲裁也分别从 `stepPb` 取出
  L1、L2、L3 请求，交给 Queued 前才写共享 filter、ArchDB trace 并记录
  `pfGenerated`。该计数表示已跨 PB 的候选，不表示已经穿过 PF control、PFQ
  replacement 或到达 cache issue 边界。
- STEP 固定要求 `use_pf_buffer=True` 和 `prefetch_train=False`。候选写入 PB
  不是 `FT.issued` 的边界；只有 PB-to-Queued 交接通过 PF-control、跨页检查和
  `Queued::insert()` 后才写该位，后续 SOE/TOE 才不重复发送。共享 filter 拒绝、
  PF-control 拒绝、跨页拒绝或队列去重拒绝都保持 FT 可发射，并由
  `bufferHandoffFilteredBlocks` 或 `bufferHandoffRejectedBlocks` 可观测。
  该位表示候选已经被当前层 Queue 接受，而非最终已经从 PFQ dequeue。论文明确
  写出 FOE 成功后更新 `issued`、SOE 检查该位；本实现将其同样用于抑制 TOE 的
  重复发射，这是本地去重策略。
- `step_pf_level=2` 是最终 L2 placement：L2 worker 对该 `STEP` source 绕过
  既有 low-accuracy L2-to-L3 offload；`step_pf_level=3` 仍使用正常 hint 链路
  抵达 L3。这样 target-level A/B 不会混入额外 L3 转发。
- 最终 block/page filter 是所有复合组件共享的去重域。STEP 在 PB handoff 时
  使用既有 `sharedFilterKey()`，与 stream/stride/SMS 使用同一 key，避免 STEP
  绕开跨源去重；更严格的 context/security 比较只在 STEP 私有表和 `stepPb` 内。

## 参数接口和 GEM5 初版默认值

论文将 STEP 主要实现为 L2C add-on；Table I 与 Sec. V-A/E/G 给出的低存储
基线为 4 KB region、FT/AT/PHT `256/128/512` entries、均为 8-way、PB 32
entries、12-bit PC hash，且主实验关闭 SOE。它不是论文全参数空间的性能最优
宣称：AT 增至 256 后才饱和，PHT 从 8-way 到 128-way 仍有小幅收益。论文并未
定义 `enable_step`、L1/L2/L3 target level 或 GEM5 PB 的 indexing policy；这些
属于下表中的 GEM5 初版实现选择，用于可控 A/B 与后续 DSE。

所有参数位于 `XSCompositePrefetcher`，保持 Python SimObject 配置可见：

| 参数 | GEM5 初版值 | 来源 | 含义 |
| --- | ---: | --- | --- |
| `enable_step` | `False` | GEM5 | 启用 STEP 并替换 SMS PHT 发射路径 |
| `step_region_size` | 4096 B | 论文基线 | STEP 的空间 region，独立于现有 SMS 的 1 KB region |
| `step_ft_entries` / `step_ft_assoc` | 256 / 8 | 论文基线 | FT 容量与相联度 |
| `step_act_entries` / `step_act_assoc` | 128 / 8 | 论文基线 | AT 容量与相联度；论文 sensitivity 中 256 entries 后饱和 |
| `step_pht_entries` / `step_pht_assoc` | 512 / 8 | 论文基线 | PHT 容量与相联度；8-way 是低存储点，不是全局最优 |
| `step_pf_buffer_entries` | 32 | 容量来自论文；全相联为 GEM5 | STEP 专用 `stepPb` 的容量；当前 indexing policy 为全相联 |
| `step_pc_hash_bits` | 12 | 论文基线 | FOE 匹配和 maturity 的 PC 哈希宽度；hash 函数为 GEM5 选择 |
| `step_confidence_entries` | 3 | 论文默认 | Jaccard 参与的最大历史项数 |
| `step_confidence_threshold` | 75% | 论文默认 | FOE/SOE 收敛阈值，比较为严格大于 |
| `step_enable_foe` / `step_enable_soe` / `step_enable_toe` | `True` / `False` / `True` | 论文主配置 + GEM5 接口 | 各触发点的发射开关；论文主配置关闭 SOE |
| `step_pf_level` | 2 | GEM5（与论文 L2C 位置对应） | 目标级别：1 为 L1，2 为 L2，3 为 L3 |

构造时断言 STEP region 为二的幂、能被 block size 整除且不超过 64 lines；
初版使用 `uint64_t` footprint，避免无界 vector 或动态分配进入热路径。

配置入口提供 `--enable-step`、`--step-enable-soe` 和
`--step-pf-level {1,2,3}`。`--enable-step` 在
`_configure_xs_composite()` 中设置 `enable_step=True`、`enable_pht=False`，
同时保留选定 profile 的 stream/stride 配置；构造函数也以
`enable_pht && !enable_step` 强制排除 legacy SMS PHT。

## 文件和当前状态

1. 已实现但尚未作为代码提交的改动包括 `step.hh/.cc`、`step.test.cc`、
   `Prefetcher.py`、`sms.hh/.cc`、`Options.py`、`PrefetcherConfig.py`、
   `Request.py` 和 prefetch `SConscript`。它们覆盖参数、source attribution、
   STEP 状态、训练、lookup 和独立 buffer。
2. `step.test.cc` 覆盖收敛/发散 footprint、重复 offset、FOE maturity、SOE/TOE
   匹配、同事件多条 history 的置信度、真实 AT victim 训练、`issued` 抑制和
   未接纳后 TOE 继续、PHT 的 PC/maturity 语义，以及 ContextID/secure 隔离。
3. `scons build/RISCV/gem5.opt --gold-linker -j1` 已通过，生成的
   `build/RISCV/gem5.opt` 为有效 ELF；`step.test.opt` 16/16、
   `context_key.test.opt` 6/6 通过。

## 可观测性和验证计划

新增统计位于 `XSCompositePrefetcher.step` 子组，覆盖每个阶段的
lookup/hit/decision/admitted blocks、FOE maturity 拒绝、FOE/SOE 低置信度
推迟、TOE miss、FT/AT 分配、PHT 插入和 PHT victim。`XSCompositePrefetcher`
debug flag 已输出 STEP 启用和 target level、每个 decision 的 region/FO/SO/TO、
匹配数、footprint 和候选 mask，以及训练的 footprint 与 maturity。当前 trace
不输出单独的数值 Jaccard 相似度；需要该证据时应扩展 trace，而不是把它误称为
已有输出。

PB 的必查统计为 `step.preBufferFilteredBlocks`、
`step.bufferHandoffFilteredBlocks`、`step.bufferHandoffRejectedBlocks`、
`step_pb.pendingEvictedBlocks` 与
`step_pb.emptyReclaims`。最终效果按 `STEP` source 的
`pfIssued_srcs`、`pfDequeued_srcs`、`pfUseful_srcs`、`pfUnused_srcs`、
`pfRemovedFull_srcs`、`pfControlDroppedBySource` 归因；`pfGenerated` 不是
成功 cache issue 的证据。

验证由低到高进行：

1. 运行现有 gtest，覆盖确定性状态转移、重复 offset、FOE maturity、TOE
   精确匹配、同事件的多 footprint history、真实 AT victim 训练、`issued` 和
   ContextID/secure 隔离。
2. 编译 `gem5.opt`，生成 `kmhv3.py` 的 config.ini，确认 `enable_step=True` 和
   所有论文初版参数实际落入 `system.cpu[0].dcache.prefetcher`。
3. 指定 `omnetpp/6881` checkpoint 的当前修订 10k/1M 诊断 smoke 已完成，
   基线与 STEP 均正常恢复并以 max instruction count 退出；10k/1M 本地 A/B
   不作为完整性能结论。
4. 提交/推送后触发 `gcc15-spec06-1.0c` + `kmhv3.py` CI，使用
   `distributed_servers=default`（展开为 `node020-node034,node036-node039`）；
   只将同一基线 SHA、相同 workload/配置的完成归档与 run `32391965338` 比较。
5. 仅在完整 CI 有正向结果后，以代表性切片、冻结 workload、明确目标和预算建立
   solver spec；DSE 触发前另行回显完整 CI 合同并取得确认。

## 已知差异与风险

- STEP 通过原始 demand hook 观察每个合格 load，仍不是 ChampSim trace 的每次
  cache access；GEM5 的 probe 时点、地址空间选择、合并和训练过滤仍可能改变
  FOE/SOE/TOE 的观测密度，属于需用 checkpoint 验证的建模差异。
- `stepPb` 是按本仓库多级发射语义工作的独立缓冲，不与论文 PB 的位级容量一一
  等价。论文只在 PQ 满时暂存并在未来访问重试；当前每个候选都进入 cycle-driven
  `stepPb`，`FT.issued` 的确认边界是本层 `Queued::insert()` 接受，不是最终从
  PFQ dequeue。PB replacement 不会把未发请求误记为已写入共享去重过滤器，因为
  共享 filter、trace 和 `pfGenerated` 都延后到 PB-to-Queued handoff；交接被拒时
  FT 可以再次由 TOE 尝试，但已经消费的 PB bit 不会被写回，故仍不具备论文完整的
  future-access retry 资格。
- 论文 10.50 KB 的计算包含 8-entry DPCT streaming；当前复用本仓库 stream/stride，
  而 `stepPb` 保存 trigger、ContextID 与 C++ 对象。实现不声称与论文完整组合或
  硬件面积逐项等价。
- 论文没有完全定义 1 到 N 之间的所有候选数情形。实现采用：FOE 单项需 maturity；
  SOE 单项可发；两项或更多项逐一与最新项比较。这一工程决策需由单测固定。
- `step_enable_soe=False` 是论文平均最优配置，不是 KMHv3 上的结论；DSE 时必须
  将其纳入离散候选。
