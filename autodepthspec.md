# PDB64 Reuse-Competitive Auto-Depth Spec

## 1. 目标与范围

本文定义一套面向 64-block PDB 的 Stream 预取深度控制器。控制器不使用 BDP、delta 或 RTU 决定深度，而是直接让两类可观察结果竞争：

- `late` 表示预取数据到达过晚，要求增加 depth。
- `unused-victim reuse` 表示预取块在第一次 demand 使用前已经被 PDB 替换，要求减小 depth。

当 depth 已达到最小档位而容量复用问题仍然存在时，控制器可以把选择出的 PDB victim 搬入 DCache：

- `used-victim reuse` 控制 used victim 的搬运。
- 最小 depth 下持续存在的 `unused-victim reuse` 控制 unused victim 的搬运。

本文只考虑一条 L1 Stream，StoreStream 触发保持关闭。物理 PDB 容量固定为 64 block，不允许通过扩大 PDB 达成性能目标。

BDP、delta 和 RTU 可以继续输出诊断统计，但在本控制模式下不得改变最终 depth、确认状态或搬运状态。

本规范冻结以下信号分工：

- `used-victim reuse` 不得进入 depth 的减浅压力，只能控制 used move；
- used move 只使用 `usedVictimHits` 启停，不定义或使用 `handledUsedVictimHits`；
- unused move 的开启资格为完整最小深度窗口中的
  `unusedPressure > 0 && unusedPressure >= latePressure`；
- late/unused 压力相等不再使用独立的 conflict 开启规则，上述不等式已经覆盖非零相等情况。

除明确标记为“已冻结”的设计不变量外，本文出现的具体数值均只是当前候选值，不能表述为已经验证的最优参数。

## 2. 强制不变量

### 2.1 观测与动作必须解耦

Victim reuse 观测器必须独立于所有控制动作并始终运行。

```text
enable_used_move = false
```

只表示 used victim 不搬入 DCache，不得禁止：

- 向 victim table 插入 used victim；
- demand 查询 victim table；
- `usedVictimHits` 计数；
- used move 的 shadow decision 和诊断统计。

```text
enable_unused_move = false
```

只表示 unused victim 不搬入 DCache，不得禁止：

- 向 victim table 插入 unused victim；
- demand 查询 victim table；
- `unusedVictimHits` 计数；
- competitive depth 控制；
- unused move 的 shadow decision 和诊断统计。

因此，“关闭两种 move 做 depth-only 实验”必须仍然得到完整的 used/unused victim reuse 数据。任何把 observer 绑定在 move enable 上的实现都不符合本规范。

### 2.2 Victim-table hit 与 cache hit/miss 无关

本文中的 reuse 定义为 demand block address 命中 victim table。控制器不查询、记录或依赖该 demand 最终在 DCache、MSHR 或下层发生 hit 还是 miss。

Victim table 必须在所有 demand 请求上查询，包括最终会命中 DCache 的请求。否则 move-to-DCache 一旦成功，就会隐藏维持搬运所需的 reuse 信号。

本规范不定义 `handledUsedVictimHit`。Used move 开启后，命中 victim table 的 demand 仍统一计为 `usedVictimHit`，不再判断该 block 是否仍驻留在 DCache。Move 分配成功、失败以及分配当拍造成的 DCache victim 可以作为诊断统计，但后续 demand 的 cache hit/miss 不参与 move 状态机。

### 2.3 每个 victim 只统计第一次 reuse

Demand 命中 victim table 后立即删除对应表项。同一个 PDB victim 最多贡献一次 reuse，避免连续访问同一 block 放大压力。

### 2.4 Victim table 不随控制窗口清空

控制窗口只读取累计计数器的差分。Victim table 跨窗口保留，正常淘汰由其自身 LRU 以及可选的最大年龄限制决定；demand 命中、重复插入和显式失效也可以删除或更新对应表项。

## 3. 术语与事件

| 名称 | 定义 |
|---|---|
| PDB refill | 一条目标 Stream 预取数据成功进入 PDB |
| PDB capacity victim | PDB 为新 block 分配空间时按 LRU 选出的有效 victim |
| used victim | victim 在 PDB 驻留期间至少被一次 demand 使用 |
| unused victim | victim 在 PDB 驻留期间从未被 demand 使用 |
| victim-table hit | victim 离开 PDB 后，后续 demand 地址命中其 victim-table 表项 |
| used-victim reuse | demand 命中 `used=1` 的 victim-table 表项 |
| unused-victim reuse | demand 命中 `used=0` 的 victim-table 表项 |
| late event | 当前 G2 定义的两类 Stream/demand 在途来晚事件之和 |
| depth epoch | 一次实际 depth 变化之间的代次编号 |

Demand 命中 PDB 行时必须把该行的 `used` 置为 1，并按现有行为把该行更新到 PDB LRU 的 MRU 位置。后续再次命中可以继续刷新 LRU，但 `used` 保持为 1；容量替换时复制这一最终状态。

### 3.1 Refill-to-use 与 victim reuse 的截断分布解释

PDB replacement 不是 RTU 观测的终点，而是一条把完整时间分布截成两部分的动态边界。令：

```text
t_refill   = block 进入 PDB 的时刻
t_replace  = block 被 PDB 容量替换的时刻
t_first    = refill 后第一次 demand 使用的时刻
t_victim   = t_replace
t_post     = victim 后第一次 demand 使用的时刻
```

对于首次使用分布：

```text
t_first < t_replace:
    RTU = t_first - t_refill

t_first > t_replace 且出现 unused-victim reuse:
    RTU = (t_replace - t_refill) + (t_post - t_victim)
        = refillToReplace + unusedVictimToDemand
```

因此，PDB 内直接观测到的 RTU 只是“在替换前已经首用”的条件分布，天然偏小；`unused-victim reuse` 才是同一个首次使用分布越过 replacement 边界后的可观测右尾。只统计事件数量可以估计右尾质量；同时保留 `refillToReplace` 与 `victimToDemand`，则可以估计完整 RTU 分布的中心相对每条 block replacement 时刻的位置。victim-table 表项在 demand 到来前先被自身 LRU 淘汰的样本仍是右删失样本，不能当成永不使用。

`used-victim reuse` 不属于首次 RTU 的右尾：该 block 在 replacement 前已经至少使用一次，victim 后的 demand 是再次使用。它描述的是容量复用窗口或相邻使用间隔，用来判断 used block 是否值得搬入 DCache。若要重建这一分布，应记录 replacement 前最后一次 PDB demand hit：

```text
nextUseDistance = (t_replace - t_last_use) +
                  (t_post - t_victim)
```

仅保存 `firstUseTick` 无法准确恢复该距离，因为后续 PDB hit 虽然刷新 LRU，却不会刷新首次使用时刻。第一版仍可用 `used-victim reuse` 的计数驱动 move；若后续用分布中心或分位数调控，应增加 `lastUseTick`，并把两类分布分别统计，不能把首次使用和再次使用样本混合。

初版 late 口径保持：

```text
lateEvents = mshrHits + demandMshrHits
```

这两类事件的精确定义沿用现有 G2。若后续修改 late 事件集合，必须建立新的 A/B，不能只调整阈值并沿用旧结论。

## 4. PDB Victim Table

### 4.1 表项

仅跟踪属于目标 L1 Stream 的 PDB capacity victim。最小功能表项为：

```cpp
struct VictimEntry
{
    Addr blockAddr;
    bool secure;
    bool used;
    uint16_t depthEpoch;
    Tick victimTick;       // 仅在启用最大年龄限制时需要
    LruState lru;
};
```

`blockAddr` 使用物理 cacheline 地址，不包含 block offset。若系统存在额外地址域，必须加入能够避免错误别名的 context 标识。

`depthEpoch` 取该 block 进入 PDB 时的 epoch，而不是它被替换时的当前 epoch。因此 PDB 行本身也需要保存 refill 时的 depth epoch，victim 时复制到 victim table。

Prefetch source 可以作为诊断字段增加，但不得改变 reuse 的基本定义。除可选最大年龄需要的 `victimTick` 外，控制器不消费 victim/reuse 延迟。Victim-table 控制字段不需要记录 moved block 后续是否仍驻留在 DCache。

### 4.2 Victim 插入

```text
on_pdb_capacity_victim(block):
    if block 不是目标 Stream: return
    key = (physical_block_address, secure/context)
    if key 已存在于 victim table:
        用最新 victim 信息覆盖旧表项
        将表项更新为 MRU
        duplicateInsertions++
    else:
        if victim table 已满:
            按 LRU 删除最老表项
            tableEvictions++
        插入 {key, used, refillDepthEpoch}
        insertions[used]++
```

只有容量替换进入控制观测。Snoop invalidation、全局 flush、Store 权限移交等非容量删除不作为 unused-reuse 或 retention 证据；它们可以单独统计。

无论 victim 最终 release，还是被 move-to-DCache，均必须插入 victim table。搬运不得使该 victim 从观察集合消失。

### 4.3 Demand 查询

```text
on_demand_request(address, secure/context):
    entry = victim_table.lookup(block_address)
    if entry 不存在: return

    if entry.used:
        usedVictimHits++
    else:
        unusedVictimHits++

    if entry.depthEpoch == currentDepthEpoch:
        currentEpochVictimHits[entry.used]++
    else:
        staleEpochVictimHits[entry.used]++

    删除 entry
```

查询发生在已知 demand 地址之后即可，不等待 cache hit/miss 结果。Victim-table hit 不改变 cache 数据路径和访问延迟。

### 4.4 LRU 的语义

Victim table 的 LRU 提供按“后续 victim 数量”度量的有限观察范围，而不是固定 cycle 时间。当前候选同时设置最大表项年龄以过滤很久以前的 victim，但该年龄尚未冻结。表项因 LRU、年龄限制被删除时只记录 `tableEvictions` 或 `ageExpirations`，不把它计为 reuse 或 no-reuse。

Victim table 容量必须做独立敏感性测试，以确认观测结果没有被过小表格严重截断。

## 5. Competitive Depth Controller

### 5.1 控制状态

```text
depthLevels       # 有序合法深度集合
depthIndex        # 当前实际档位
depthEpoch        # 每次实际改档后递增
upCredit          # 加深方向确认积分
downCredit        # 减浅方向确认积分
settleRemaining   # 改档后的稳定窗口数
refillSnapshot    # 上一个窗口边界的累计 refill
eventSnapshots    # late/reuse 累计计数快照
```

当前候选值如下，具体档位和初始值尚未冻结：

```text
depthLevels = [4, 8, 16, 24, 32, 48, 64]
initialDepth = 16
```

depth 始终直接取合法档位，不再计算 `bdpDepth + delta`。

### 5.2 窗口

当前候选控制窗口由目标 Stream 的 500 次成功 PDB refill 推进；refill 作为推进事件已经冻结，窗口长度 500 尚未冻结：

```text
windowRefills = 500
```

窗口结束时使用累计计数器差分：

```text
windowLate = cumulativeLate - lateSnapshot
windowUnusedReuse =
    cumulativeCurrentEpochUnusedReuse - unusedReuseSnapshot
```

Victim table 不清空。窗口计数和表格生命周期彼此独立。

当前 epoch 之外的 unused reuse 只进入诊断统计，不参与当前 depth 决策。Late 事件也应携带预取生成/发送时的 depth epoch；无法归属当前 epoch 的 late 只进入诊断统计。

### 5.3 压力计算

固定分母为 `windowRefills`：

```text
lateRate = windowLate / windowRefills
unusedRate = windowUnusedReuse / windowRefills
```

两个方向分别量化，低事件率不会产生相反方向压力：

```text
quantize(count, low, high):
    if count <= low: return 0
    if count < high: return 0.5
    return 1

latePressure = quantize(windowLate, lateLow, lateHigh)
unusedPressure = quantize(windowUnusedReuse, unusedLow, unusedHigh)
netPressure = latePressure - unusedPressure
```

当前候选在 500-refill 窗口中使用 late `20/40` 和 unused `40/80`。两组阈值均未冻结，必须独立探索；`usedVictimHits` 不得以任何形式并入 `unusedPressure` 或其他 depth 压力。

### 5.4 积分与改档

```text
if settleRemaining > 0:
    settleRemaining--
    upCredit = 0
    downCredit = 0
    reason = settling
else if netPressure > 0:
    upCredit += netPressure
    downCredit = 0
else if netPressure < 0:
    downCredit += -netPressure
    upCredit = 0
else:
    upCredit = 0
    downCredit = 0
    reason = hold_or_equal

if upCredit >= creditThreshold and depth 不是最高档:
    depth = higher(depth)
    depthEpoch++
    settleRemaining = settleWindows
    upCredit = downCredit = 0

if downCredit >= creditThreshold and depth 不是最低档:
    depth = lower(depth)
    depthEpoch++
    settleRemaining = settleWindows
    upCredit = downCredit = 0
```

当前候选使用 `creditThreshold=1`：压力 1 可以单窗口改档，压力 0.5 需要两个连续同向窗口；每次最多移动一档。这两个参数均未冻结。

如果 late 与 unused 压力相等，depth 保持不变。非零相等窗口可以保留为诊断统计；它不是独立控制机制。在完整最小深度窗口中，非零相等自然满足 unused move 的开启资格。

### 5.5 Epoch 与稳定期

改档后必须递增 epoch。旧 epoch 的 victim-table hit 继续进入累计统计，但不能驱动新 depth 再次改档。

第一个稳定窗口不做新决策，用于排出旧 depth 的在途请求和 PDB resident。稳定期不清空 victim table，只限制控制器消费哪些事件。

严格实现应把 epoch 随 Stream 请求传到 late 事件，并把 refill epoch保存在 PDB 行中。仅依赖一个等待窗口而不做事件归属，可能把旧 depth 的延迟反馈错误归因到新 depth。

## 6. Used-Victim Move Controller

Used-victim move 与 depth 控制相互独立。它只使用当前 500-refill 窗口的 `windowUsedVictimHits`，不使用 demand cache miss。

```text
state = OFF

OFF:
    windowUsedVictimHits >= usedMoveOnHits
    连续 usedMoveOnWindows
    -> 下一窗口 ON

ON:
    windowUsedVictimHits <= usedMoveOffHits
    连续 usedMoveOffWindows
    -> 下一窗口 OFF
    否则保持 ON
```

必须满足：

```text
usedMoveOffHits < usedMoveOnHits
```

形成迟滞。开启后 reuse 信号不会因为搬运成功而消失，因为所有 demand 都查询 victim table。

`windowUsedVictimHits` 包含所有符合 epoch 规则的 used-victim reuse，不区分该 demand 最终是否命中 DCache。Used move 的开启、保持和关闭均不得依赖 `handledUsedVictimHits`、DCache residency 或 demand cache hit/miss。当前候选使用 `20` hits、连续 `3` 窗开启，使用不超过 `5` hits、连续 `2` 窗关闭；这四个数值均未冻结。

PDB 选择 used capacity victim 时：

```text
if enableUsedMove and usedMoveState == ON:
    尝试把 victim 数据写入 DCache
else:
    执行原 release 路径

无论选择哪条路径：
    把 victim 插入 victim table
```

搬运失败时执行原 release 路径，并记录失败原因。搬运成功率和 DCache 置换只用于诊断及性能判断，不参与 reuse 的定义。

## 7. Unused-Victim Move Controller

Unused victim 优先通过减小 depth 处理。只有一个完整窗口开始和结束时都处于最小 depth，才允许该窗口为 unused move 累积开启确认。

```text
fullMinDepthWindow =
    windowStartDepth == minDepth && windowEndDepth == minDepth

OFF:
    fullMinDepthWindow
    unusedPressure > 0
    unusedPressure >= latePressure
    连续 unusedMoveOnWindows
    -> 下一窗口 ON

ON:
    unusedPressure == 0
    连续 unusedMoveOffWindows
    -> OFF

    其他情况保持 ON
```

`unusedPressure > 0` 是必要条件，防止 `0 >= 0` 在没有任何 unused reuse 证据时开启搬运。`unusedPressure >= latePressure` 表示减浅方向占优或与加深方向相等，而 depth 已没有更浅档位可用。该规则统一覆盖原来的强 unused 压力和非零 late/unused 相等情形，不再设置独立的 `unused_move_on_conflict`。

在量化压力集合 `{0, 0.5, 1}` 下，开启资格如下；表中还必须同时满足完整最小深度窗口和连续窗口要求。

| `unusedPressure` | `latePressure=0` | `latePressure=0.5` | `latePressure=1` |
|---:|:---:|:---:|:---:|
| 0 | 否 | 否 | 否 |
| 0.5 | 是 | 是 | 否 |
| 1 | 是 | 是 | 是 |

当前候选允许 unused move 开启后跨越 depth 变化继续保持，并在连续低 unused 压力窗口后关闭。是否应在离开最小 depth 时立即关闭尚未冻结，必须与 persistent 方案做 A/B。Unused move 状态机和 depth 控制都只消费 victim-table reuse，不使用 moved block 后续是否仍在 DCache 的信息。

PDB 选择 unused capacity victim 时：

```text
if enableUnusedMove and unusedMoveState == ON:
    尝试把 victim 数据写入 DCache
else:
    执行原 release 路径

无论选择哪条路径：
    把 victim 插入 victim table
```

Unused move 存在明确的 DCache 污染风险。即使 victim 后来大量 reuse，搬运成本仍可能超过避免的访存成本，因此必须作为独立 A/B 阶段验证，不得与 depth 参数同时联合调优后直接归因。

## 8. 开关语义

建议增加四个独立开关：

| 开关 | 含义 |
|---|---|
| `enable_reuse_observer` | 启用 victim table、demand 查询和全部 reuse 统计 |
| `enable_competitive_depth` | 允许 late/unused 竞争改变 depth |
| `enable_used_move` | 允许 used move 状态实际搬运 victim |
| `enable_unused_move` | 允许 unused move 状态实际搬运 victim |

实验约束：

```text
enable_reuse_observer = true
```

必须在所有 competitive-depth 实验中成立。若任一 move 打开而 observer 关闭，配置应直接报错。

Move 开关关闭时，状态机仍应计算 shadow decision，并输出 `wouldEnable`、`wouldMove` 等诊断计数，只禁止实际数据搬运。

## 9. 参数与冻结状态

参数状态的含义如下：

- **已冻结**：当前算法定义的一部分，后续 DSE 不应改变，除非显式重开设计讨论；
- **未冻结**：只是当前候选值，已有局部数据也不能视为全局最优；
- **待决定**：连机制语义仍需 A/B，不能作为正式候选的默认结论。

### 9.1 当前参数表

| 参数或规则 | 当前值 | 状态 | 说明 |
|---|---:|:---:|---|
| `pdb_entries` | 64 | **已冻结** | 目标物理容量，不允许扩大容量换取性能 |
| victim table 生命周期 | 跨窗口、LRU | **已冻结** | 窗口结束不清表，demand 首次命中后删除 |
| victim-table hit 定义 | 仅地址命中 | **已冻结** | 不依赖 demand 的 cache hit/miss |
| `victim_table_entries` | 256 | **未冻结** | 64 太小已有负面证据；最终 RTL 容量仍需敏感性测试 |
| `victim_max_age_cycles` | 16384 | **未冻结** | 当前候选的旧证据过滤年龄 |
| `min_depth` | 4 | **已冻结** | unused move 只能由完整 depth=4 窗口开启 |
| `competitive_depth_levels` | `4,8,16,24,32,48,64` | **未冻结** | 与旧 G2 的 `depth_levels` 分离；80/96 尚未在最终控制器上验证 |
| `initial_depth` | 16 | **未冻结** | 尚无最终机制下的启动敏感性结论 |
| 窗口推进事件 | 成功 PDB refill | **已冻结** | 不使用 prefetch sent 计数 |
| `window_refills` | 500 | **未冻结** | 所有现有 competitive DSE 都固定为 500 |
| `late_low/high` | `20/40` | **未冻结** | 在 500-refill 窗口中对应约 4%/8% |
| `unused_low/high` | `40/80` | **未冻结** | 当前候选值；不得混入 used reuse |
| `credit_threshold` | 1 | **未冻结** | 尚未做最终机制下的阈值扫描 |
| `settle_windows` | 1 | **未冻结** | 旧版本只比较过 1/2，未在最终机制重测 |
| `max_level_step` | 1 | **未冻结** | 尚未比较强证据下一次两档 |
| used reuse 参与 depth | 禁止 | **已冻结** | 删除 `usedEarlyPressure` 路径 |
| used move 启停证据 | 仅 `usedVictimHits` | **已冻结** | 开启、保持和关闭均只看 victim-table reuse |
| used move cache-outcome 信息 | 不使用 | **已冻结** | 删除 `handledUsedVictimHit` 及其维护参数 |
| `used_move_on_hits` | 20 | **未冻结** | 需要与开启连续窗数联合验证 |
| `used_move_off_hits` | 5 | **未冻结** | 目前从未独立扫描 |
| `used_move_on_windows` | 3 | **未冻结** | 比 1/2 窗更安全，但未完成阈值二维 DSE |
| `used_move_off_windows` | 2 | **未冻结** | 目前从未独立扫描 |
| unused move 开启资格 | `U>0 && U>=L` | **已冻结** | `U=unusedPressure`，`L=latePressure`，且要求完整 depth=4 窗口 |
| 独立 conflict 开启规则 | 删除 | **已冻结** | 非零相等已由 `U>0 && U>=L` 覆盖 |
| `unused_move_on_windows` | 3 | **未冻结** | 1/2/3 窗的旧规则数据支持 3，但新资格规则增加了 `U=0.5,L=0` 情形 |
| unused move 关闭证据 | `unusedPressure==0` | **待决定** | 尚未在新开启规则下验证关闭语义 |
| `unused_move_off_windows` | 4 | **未冻结** | 2/4/8 只在较早机制上比较过 |
| `persistent_unused_move` | true | **待决定** | 需要与离开 depth=4 即关闭做严格 A/B |
| move DCache 插入优先级 | normal | **未冻结** | unused low-priority 已被否决；used 侧仍有混合结果 |

### 9.2 必须完成的 DSE

优先围绕本规范中的最终信号分工探索，不能沿用旧代码阶段的最优点直接冻结：

1. `late_low/high` 与 `unused_low/high`：先独立扫描，再验证少量交互组合。
2. `competitive_depth_levels`：至少补测最大 depth 80/96，并检查 24/48 档位的必要性。
3. `settle_windows`、`credit_threshold`、`max_level_step`：在相同阈值和 move 配置下比较。
4. `window_refills`：比较不同窗口长度时必须按事件率同比缩放 late/unused 阈值。
5. `initial_depth`：至少比较 8/16/24，并同时检查 warmup 继承到 ROI 的状态。
6. Used move：联合扫描 on hits、on windows、off hits、off windows，不能只做单变量历史拼接。
7. Unused move：在新 `U>0 && U>=L` 资格规则下重新比较 on/off windows，并验证新增的 `U=0.5,L=0` 区域。
8. `persistent_unused_move`：比较跨 depth 保持与离开 min depth 立即关闭。
9. Victim table：在最终 depth/move 状态机上联合比较容量和最大年龄，至少覆盖 128/256/512 与 8K/16K/32K。

完整笛卡尔积没有必要，但任何标记为“未冻结”的参数在定稿前必须有最终机制下的直接对照证据。阶段性 A/B、旧版本 DSE 和单个 workload 收益只能用于缩小搜索范围。

## 10. 统计要求

### 10.1 Observer 统计

```text
victimTableInsertionsUsed
victimTableInsertionsUnused
victimTableDuplicateInsertions
victimTableEvictions
victimTableOccupancy
usedVictimHits
unusedVictimHits
currentEpochUsedVictimHits
currentEpochUnusedVictimHits
staleEpochUsedVictimHits
staleEpochUnusedVictimHits
```

### 10.2 Depth 控制统计

```text
competitiveWindows
windowRefills
windowLateEvents
windowUnusedVictimHits
latePressure
unusedPressure
netPressure
upCredit
downCredit
depthIncreases
depthDecreases
settleWindows
equalNonzeroPressureWindows
currentDepth
currentDepthEpoch
windowsAtDepth::<level>
```

### 10.3 Move 统计

```text
usedMoveEnables / usedMoveDisables / usedMoveActive
unusedMoveEnables / unusedMoveDisables / unusedMoveActive
usedMoveWouldEnable / usedMoveWouldMove
unusedMoveWouldEnable / unusedMoveWouldMove
usedMoveAttempts / usedMoveSuccesses / usedMoveFailures
unusedMoveAttempts / unusedMoveSuccesses / unusedMoveFailures
```

还应保留 DCache 中“预取/搬运行替换 demand 行”等污染统计，但这些统计不进入第一版控制状态机。

不得为控制 used move 增加 `handledUsedVictimHits`、`maintenanceHandledUsedVictimHits` 或等价的 DCache 后续驻留统计。若模拟器为了离线归因保留同类实验性计数，也必须与正式控制接口隔离，并且不能成为 RTL 必需状态。

所有统计必须在实际 move 关闭时仍可读取。验证脚本应把“move 关闭且 reuse 计数始终为零”视为可疑配置或实现错误，而不是正常结果。

## 11. 实现位置

建议按现有模块边界实现：

- `src/mem/cache/cache.hh/.cc`
  - victim table；
  - PDB capacity victim 插入；
  - 所有 demand 的 victim-table 查询；
  - used/unused victim 的可选 DCache 搬运；
  - observer 与 move 统计。
- `src/mem/cache/prefetch/base.hh` 及 composite/forwarder 接口
  - PDB victim/reuse 通知；
  - depth epoch 传递；
  - move decision 查询。
- `src/mem/cache/prefetch/xs_stream.hh/.cc`
  - refill 窗口；
  - late/unused 压力；
  - depth 状态机；
  - used/unused move 状态机；
  - epoch 和控制统计。
- `src/mem/cache/prefetch/Prefetcher.py`
  - 功能开关和所有可调参数。

初版应通过 `enable_competitive_depth` feature flag 旁路原 G2 的 BDP/delta/RTU 决策，不立即删除旧代码。这样可以使用同一源码进行严格 A/B，并验证新模式下旧控制器的变化计数始终为零。

竞争控制必须使用独立的 `competitive_depth_levels`。旧 G2 继续使用原有 `depth_levels`，关闭 `enable_competitive_depth` 时不得因新算法候选档位而缩小旧 G2 的搜索范围。旧 G2 实际改变 depth 时仍需递增 depth epoch，并丢弃尚未完成的竞争观察窗口，防止 observer 把两个 depth 的事件混在同一窗口。

## 12. 调参与验证顺序

### 阶段 0：Passive Observer

```text
competitive depth = OFF
used move = OFF
unused move = OFF
reuse observer = ON
```

要求与无 observer 基线逐 checkpoint 周期完全一致或解释所有差异。收集 victim-table 容量截断、used/unused reuse 和 epoch 分布。

### 阶段 1：Depth Only

```text
competitive depth = ON
used move = OFF
unused move = OFF
reuse observer = ON
```

这是调节 late/unused 阈值、初始 depth、档位和稳定期的唯一阶段。虽然两种 move 关闭，所有 reuse 数据必须继续统计，两个 move 状态机也应输出 shadow decision。

先在代表性 checkpoint 和稳定对照上验证，再跑完整 focus 集。需要检查的不只是性能，还包括每档窗口、改档路径、非零相等压力窗口和旧 epoch 事件。

### 阶段 2：Used Move

冻结阶段 1 的全部 depth 参数，只打开 used move。根据阶段 1 的 passive `usedVictimHits` 分布确定 on/off 阈值，对 used move 做严格 A/B。Used move 的关闭同样只使用 `usedVictimHits`；不得引入 handled hit 或 demand cache 结果。

### 阶段 3：Unused Move

继续冻结 depth 和 used move 参数。只有连续完整 depth=4 窗口满足 `unusedPressure > 0 && unusedPressure >= latePressure` 时才允许打开 unused move。单独验证性能收益、DCache 污染和搬运量；不得用同时调整 unused 阈值来掩盖 unused move 的回退。

### 阶段 4：表容量与完整覆盖

机制稳定后再缩小 victim table，选择可实现容量。最终候选必须在相同 1094-checkpoint Cov10、PDB64 配置下验证，并与原始 G2/PDB64、固定 depth 和原始 G2/PDB1024 使用相同评分口径比较。

## 13. 验收条件

功能验收至少包括：

- 配置中物理 PDB 容量确认为 64。
- Competitive 模式下 BDP、delta 和 RTU 不改变 depth。
- 两种 move 关闭时 victim table 和全部 reuse 统计仍持续工作。
- Victim table 在 DCache hit 的 demand 上也能命中。
- 每个 victim 最多统计一次 reuse。
- Move 后仍插入 victim table。
- Depth 改变后旧 epoch reuse 不驱动新 depth。
- depth 可以在持续 unused 压力下完成 `16 -> 8 -> 4`，也可以在 late 压力下逐档恢复。
- 只有完整 depth=4 窗口才能使 unused move 从 OFF 转为 ON；若最终选择 persistent 方案，ON 状态可在其他 depth 继续执行。
- `unusedPressure=0` 时不得因为 `0 >= latePressure` 开启 unused move。
- Used reuse 不得进入 depth 压力。
- Used move 不依赖 demand cache miss。
- Used move 的启停只依赖 `usedVictimHits`，不依赖 handled hit 或 DCache 后续驻留。
- Observer-only 模式不改变模拟行为。

性能验收必须报告逐 benchmark、重点 checkpoint、全局得分、每档驻留、reuse、late、搬运量和 DCache 污染。局部 focus 提升不能替代完整 Cov10 结果。

## 14. 尚未冻结的问题

- late 与 unused 的最终分段阈值及相对代价；
- 窗口长度、积分阈值、稳定窗口和最大单次步数；
- depth 6、80、96 以及 24/48 中间档位的必要性；
- 初始 depth 对 warmup 和短 phase 的影响；
- used move 的四个启停阈值/连续窗口参数；
- unused move 的开启/关闭连续窗口数；
- unused move 应依据何种低 reuse 证据关闭；
- unused move 开启后是否跨 depth 保持；
- victim table 的容量、最大年龄、相联度、端口和时序；
- used move 的 DCache 插入优先级；
- DCache 搬运失败及同拍 demand/victim 事件的 RTL 仲裁顺序。

独立 conflict 开启规则、used reuse 参与 depth、handled used reuse 控制 move 已经从设计中删除，不再列为待调参数。其余未冻结项必须通过最终机制下的独立实验解决，不能在 spec 中写成已验证结论。
