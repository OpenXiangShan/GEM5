# STEP 在 KMHv3 GEM5 中的实施方案

## 范围和建模合同

本实现把 STEP 作为 L1 `XSCompositePrefetcher` 中 SMS PHT 的可选替代
路径。`enable_step=False` 保留现有 `sms.cc` 行为；`enable_step=True`
保留复合预取器的 stream/stride、训练入口、去重和发射通道，但用 STEP
产生空间 footprint 候选。这使 A/B 只改变空间 footprint 的决策机制，
不把 Berti、XS stream 或 L2/L3 转发变化混入归因。

性能因果链为：

```text
符合现有 SMS 训练条件的 demand 访问
  -> STEP FT/AT/PHT 状态与 FOE/SOE/TOE 决策
  -> XSComposite 的地址去重和 pf buffer
  -> cache/L2 hint 的请求、MSHR/带宽竞争与需求 miss
  -> IPC、prefetch useful/unused、STEP 分阶段统计
```

模型保留的细粒度行为是 region 内前三个不同 cache-line offset 的顺序、
PHT 容量/组相联替换、FOE/SOE/TOE 的发射或推迟、以及候选进入既有队列
后的背压。它不复刻 ChampSim 的独立 PB；当前 `sms_pfFilter` 和 `Queued`
队列已经承担候选缓冲、去重、优先级和跨级发射，重复实现会产生两个不一致
的背压模型。

## 当前 SMS 与 STEP 的接口差异

`src/mem/cache/prefetch/sms.cc` 当前路径的 `ACTEntry` 在 region 访问中
累积 `regionBits`，并在 `updatePht()` 中写入由 PC/context 索引的
`PhtEntry::hist`。`hist` 是相对首 offset 的饱和计数器向量，`phtLookup()`
只命中一条聚合项。它适合当前 SMS 置信度语义，却没有多条独立 footprint
或最近顺序，不能实现论文的候选相似度。

STEP 因此使用独立的、固定容量的状态，并只复用下列现有公共能力：

| 现有能力 | STEP 的使用方式 |
| --- | --- |
| `calculatePrefetch()` 的 miss / prefetch-first-hit 训练时机 | 驱动 STEP 的观察和学习 |
| `contextKey()`、`pfi.contextId()`、secure bit | 对 FT/AT/PHT 状态保持上下文隔离 |
| `AssociativeSet`、索引与替换策略 | 实现固定容量、参数化的 FT/AT/PHT |
| `sendPFWithFilter()` | 复用 block/page 去重、ArchDB trace、priority 与 L2/L3 hint 标记 |
| `sms_pfFilter` / `Queued` | 复用预取缓冲和发射仲裁，不新增第二个 PB |

## 新状态和转移

### STEP FT

`StepFilterEntry` 以 4 KB region 地址和 ContextID 为键，保存 `firstOffset`、
`secondOffset`、`pcHash`、`issued`。它只跟踪还未进入 STEP AT 的 region。

- 首个不同 block offset：记录 FO，做 FOE lookup。
- 第二个不同 block offset：记录 SO；若尚未发射且 `step_enable_soe=True`，
  做 SOE lookup。
- 第三个不同 block offset：做 TOE lookup；随后分配 STEP AT，初始化 footprint
  为前三个 offset 的三个位，并使 FT 项失效。
- 同一 block 的重复观察不推进事件，避免由 cache/replay 的重复通知伪造时序。

### STEP AT

`StepActiveEntry` 以 region 地址和 ContextID 为键，保存前三个 offset、
12-bit PC hash、64-bit footprint 和 unique-offset 数。后续不同 offset 仅置位
footprint。AT victim 驱逐时训练 PHT；少于三个不同 offset 的 region 不写入
PHT。这等价于论文“第三次访问后进入 AT”的学习门槛。

### STEP PHT

`StepPatternEntry` 以 FO 选择组，保存 FO/SO/TO、完整 64-bit footprint、
PC hash、maturity、ContextID 和单调 `sequence`。因为同一个 FO 要保留多条
原始历史，lookup 仅扫描该 set 的候选 ways；默认 8-way，因此每个访问的
复杂度是 `O(step_pht_assoc)`，而非全表扫描。

候选选择按最新 `sequence` 取最多 `step_confidence_entries` 条。FOE 需要
FO+PC hash，SOE 需要 FO+SO，TOE 需要 FO+SO+TO。PHT 插入前保存 victim
的 PC hash；其与新项相等时将新项标为 mature，复现论文的冷启动近似。

### 决策和发射

- FOE/SOE：候选为空则不发射；单项 FOE 还必须成熟；多项使用 Jaccard
  阈值，成功时对 footprint 求交集。
- TOE：选最新精确匹配项，发射其完整 footprint。
- 发射前移除当前已经观察到的 offset，剩余候选逐 line 调用
  `sendPFWithFilter()`，保留既有去重和目标 level 语义。
- 首次成功发射后写 `FT.issued`，后续 SOE/TOE 不重复发送。低置信度不会
  设置该位，因此可继续等待。

## 参数接口和论文默认值

所有参数位于 `XSCompositePrefetcher`，保持 Python SimObject 配置可见：

| 参数 | 初版默认值 | 含义 |
| --- | ---: | --- |
| `enable_step` | `False` | 启用 STEP 并替换 SMS PHT 发射路径 |
| `step_region_size` | 4096 B | STEP 的空间 region，独立于现有 SMS 的 1 KB region |
| `step_filter_entries` / `step_filter_assoc` | 256 / 8 | FT 容量与相联度 |
| `step_act_entries` / `step_act_assoc` | 128 / 8 | AT 容量与相联度 |
| `step_pht_entries` / `step_pht_assoc` | 512 / 8 | PHT 容量与相联度 |
| `step_pc_hash_bits` | 12 | FOE 匹配和 maturity 的 PC 哈希宽度 |
| `step_confidence_entries` | 3 | Jaccard 参与的最大历史项数 |
| `step_confidence_threshold` | 75% | FOE/SOE 收敛阈值 |
| `step_enable_soe` | `False` | 是否允许 SOE 发射；论文主配置关闭 |
| `step_pf_level` | 2 | 复用现有 L2 hint 目标级别 |

构造时断言 STEP region 为二的幂、能被 block size 整除且不超过 64 lines；
初版使用 `uint64_t` footprint，避免无界 vector 或动态分配进入热路径。

## 文件和提交划分

1. 本文档与背景、进度文档：只记录设计，不改变功能。
2. `Prefetcher.py`、`sms.hh`、`sms.cc`：参数、STEP 状态、训练、lookup、统计。
3. 针对性 unit/smoke 测试：验证事件顺序、Jaccard gate、maturity、TOE 精确命中、
   `issued` 去重、AT 驱逐训练和关闭开关的旧路径。
4. 构建和 checkpoint 证据：配置、日志、stats；没有通过前不触发整套性能 CI。

## 可观测性和验证计划

新增 `XSCompositePrefetcher` stats 应至少覆盖：每个阶段的 lookup/hit/发射、
FOE maturity 拒绝、FOE/SOE 低置信度推迟、TOE miss、AT/PHT 插入和 PHT victim、
以及 STEP 实际发出的 line 数。`XSCompositePrefetcher` debug flag 输出 region、
三个 offset、候选数、相似度结论、交集 footprint 和发射阶段。

验证由低到高进行：

1. 单元或 helper 测试覆盖确定性状态转移和计算公式。
2. 编译 `gem5.opt`，生成 `kmhv3.py` 的 config.ini，确认 `enable_step=True` 和
   所有论文初版参数实际落入 `system.cpu[0].dcache.prefetcher`。
3. 对指定 `omnetpp/6881` checkpoint 先跑短窗口 A/B，再跑同长度完整单点；
   核对退出状态、difftest、STEP stats、发射轨迹和无 STEP 时的基线等价性。
4. 单点正确性成立后，提交/推送后触发 `gcc15-spec06-1.0c` + `kmhv3.py` CI；
   只将同一基线 SHA、相同 workload/配置的完成归档与 run `32391965338` 比较。
5. 仅在完整 CI 有正向结果后，以代表性切片、冻结 workload、明确目标和预算建立
   solver spec；DSE 触发前另行回显完整 CI 合同并取得确认。

## 已知差异与风险

- 当前训练入口只看到 `cache miss` 或 `prefetch first hit`，不是 ChampSim 的每次
  cache access；这会改变 FOE/SOE/TOE 的观测密度，属于需用 checkpoint 验证的
  GEM5 建模差异。
- 现有 `sms_pfFilter` 是按本仓库多级发射语义工作的缓冲，不与论文的 PB 位级
  容量一一等价；本实现保留更重要的“候选不因瞬时 queue 满而直接丢失”因果链。
- 论文没有完全定义 1 到 N 之间的所有候选数情形。实现采用：FOE 单项需 maturity；
  SOE 单项可发；两项或更多项逐一与最新项比较。这一工程决策需由单测固定。
- `step_enable_soe=False` 是论文平均最优配置，不是 KMHv3 上的结论；DSE 时必须
  将其纳入离散候选。
