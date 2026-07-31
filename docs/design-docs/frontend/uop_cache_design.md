# gem5 O3 uop cache 设计与实现

## 1. 文档目的

本文解释当前 gem5 O3 CPU 中 uop cache 的设计、数据流和时序模型。目标读者不需要
预先了解 uop cache，但应当知道处理器前端会取指、译码，再把指令送往后端执行。

这里的“uop cache”更准确地说是 **decoded-instruction cache**：它按 PC 保存 gem5 已经
译码得到的 `StaticInstPtr`。再次遇到相同代码时，Fetch 可以直接复用译码结果，不再从
I-cache 取得指令字节并调用 decoder；条件允许时，还可以直接把动态指令送到 Decode
内部的 bypass 队列，绕过普通 Fetch-to-Decode time buffer。

当前实现适合回答以下性能问题：

- 如果热点指令的译码结果能够被缓存，I-cache 和普通译码路径的压力能降低多少？
- 如果命中指令还能绕过普通 Fetch-to-Decode 延迟，前端供给能力能提高多少？
- 哪些 workload 对重复执行的小段代码、I-cache 等待或前端延迟更敏感？

它还不是一份可直接映射到 RTL 的精确硬件模型。第 10 节会集中说明其中偏理想的部分。

## 2. 为什么需要 uop cache

普通前端反复执行一段热点代码时，每次都要完成相似的工作：

1. 分支预测器给出下一段 fetch target。
2. Fetch 向 I-cache 请求包含目标 PC 的指令字节。
3. I-cache 返回数据并填充 fetch buffer。
4. decoder 根据 16/32-bit RISC-V 编码生成 `StaticInst`。
5. Fetch 创建 `DynInst`，经 fetch queue 和时序 buffer 送往 Decode。

循环会让同一批指令字节被反复读取和译码。uop cache 在首次正常译码后保存结果，后续
命中时直接从第 4 步之后继续。它可能带来四类优势：

### 2.1 缩短前端延迟

当前配置的普通 Fetch-to-Decode 延迟为 3 cycle。成功进入 bypass 队列的命中指令在
下一 cycle 就能被 Decode 看到，因此在无其他阻塞时，固定缩短 2 cycle。完整推导见
第 8 节。

### 2.2 避免 I-cache 等待

当整个预测 fetch block 都在 uop cache 中时，Fetch 可以跳过该 block 的 I-cache 请求。
I-cache hit 时，这主要减少访问和前端流水工作；I-cache miss、MSHR 冲突或端口繁忙时，
还可能避免不定长的等待。这部分收益不是固定拍数，取决于 cache 和内存系统状态。

### 2.3 降低重复译码工作

命中后直接复用 `StaticInstPtr`，不再把指令字节送入 RISC-V decoder。这可以表达真实
硬件中“decoded uop 被重复使用”的方向性收益，也能减少模拟器自身的译码工作。

### 2.4 提高热点代码的前端供给稳定性

紧循环、频繁调用的小函数和稳定控制流通常有较高的时间局部性。只要 working set 能
留在 uop cache 中，它们对 I-cache 状态的敏感度会降低，Decode 更容易连续获得指令。
收益最终仍受分支预测、Decode 宽度、Rename/后端背压和错误路径浪费限制。

## 3. 建模合同

理解统计结果前，应先明确当前模型保证什么、不保证什么。

### 3.1 输入和输出

- 输入是 FTQ 当前预测 block、待取指 PC、线程的执行上下文，以及正常 Decode 得到的
  `DynInst`。
- cache 命中输出是保存的 `StaticInstPtr`、原 PC 和是否为压缩指令。
- Fetch 仍然创建新的 `DynInst`，分配新的 sequence number，并保留预测、squash、融合
  和后端执行所需状态。uop cache 不缓存动态执行结果。

### 3.2 功能约束

- 只有整个预测 block 从 `startPC` 到实际 `target_end` 的每条指令都存在时，block 才
  算命中；部分命中不会供给该 block。
- 命中必须同时满足 **PC 匹配** 和 **`UopCacheContext` 匹配**。PC 只说明“正在取哪一
  个虚拟地址”，而不一定说明这个地址当前映射到哪段代码、以什么特权权限执行；因此
  不能只用 PC 做 key。
- macro-op 和 micro-op 不会写入本实现的 `UopCache::cache`，也不会作为命中结果从该
  cache 读出。它们仍沿用普通 Fetch/decoder 路径处理。
- trace mode 不使用 uop cache。
- 命中不改变分支预测结果；Fetch 仍以 FTQ 给出的 block 边界和预测 taken target 为准。

### 3.3 `UopCacheContext` 为什么必须参与匹配

每个 `UopEntry` 都保存一个 `UopCacheContext`。当前上下文包含五个值：

| 字段 | 表示的执行环境 |
| --- | --- |
| `prv` | 当前 RISC-V 特权级，例如 U/S/M |
| `virt` | 是否处于虚拟化执行模式 |
| `satp` | 当前 host/普通地址翻译上下文（包含页表根和地址转换模式） |
| `vsatp` | guest 地址翻译上下文 |
| `hgatp` | hypervisor 对 guest 物理地址的二级翻译上下文 |

例如，两个执行片段可能使用同一个虚拟 PC，但由于进程切换、页表切换、guest 切换或
特权级变化，PC 最终对应的物理代码和权限检查不同。如果只比较 PC，旧环境中译码的
`StaticInstPtr` 就可能被错误地复用到新环境。查询时 `UopCache::findWay()` 会同时
比较 entry 的 tag、首条指令 PC 和完整 `UopCacheContext`；任何一个字段不同都视为
miss。这样做的目的不是判断指令是否“执行过”，而是把译码结果绑定到生成它时的地址
翻译和权限环境。

需要注意，这只是当前模型选择的上下文保护集合，不等于完整的硬件一致性协议。当前已
在 `fence.i`、TLB/page-table invalidation 和 PMP 更新时失效 cache；外部 I-cache
coherence 和未遵守 `fence.i` 合同的代码修改仍不在模型保证范围内，详见第 10.8 节。

### 3.4 性能因果链

模型表达的因果关系是：

```text
重复执行已译码代码
    -> block/PC/context 命中
    -> 跳过 I-cache 字节供给和 decoder
    -> bypass 队列有空间且不破坏程序顺序
    -> 绕过普通 fetch queue 到 FetchStruct 的延迟
    -> Decode 更早获得指令
    -> 在前端确为瓶颈时降低总周期数
```

命中率高并不必然等于 IPC 提升。若瓶颈在错误分支、执行单元、数据 cache 或提交带宽，
提前供给的指令可能只是在队列中等待。

## 4. Cache 组织与参数

主要参数定义在 `src/cpu/o3/BaseO3CPU.py`：

| 参数 | 默认值 | 含义 |
| --- | ---: | --- |
| `hasUopCache` | `False` | 是否启用 decoded uop cache；关闭时 Decode 使用原有队列、背压与 SMT 仲裁 |
| `uopCacheWayNum` | `4` | 组相联 way 数，必须为 2 的幂 |
| `uopCacheSetNum` | `384` | set 数，可以不是 2 的幂 |
| `uopCacheMaxInstBytesPerEntry` | `4` | 单 entry 名义指令字节容量，必须至少为 4B |
| `uopCacheBypassQueueSize` | `64` | 每线程 Decode bypass 队列容量 |

`kmhv3.py` 和 `idealkmhv3.py` 现在默认关闭 uop cache。只有显式传入
`--enable-uop-cache` 时才开启：

```text
--enable-uop-cache
```

两种配置当前都把 `fetchQueueSize` 设为 64、`fetchToDecodeDelay` 设为 3、
`decodeWidth` 设为 8。`idealkmhv3.py` 还显式设置 4 way、384 set、每 entry 4B；
`kmhv3.py` 使用 `BaseO3CPU.py` 中相同的默认值。

当前配置合同明确禁止同时启用 SMT 和 uop cache。关闭 uop cache 时保留原有 Decode
buffer 深度、feedback backpressure、`SmtActiveThreadArbiter` 和逐线程统计更新；开启
后虽仍使用 per-thread bypass deque 和原有 SMT arbiter，但 Fetch/FTQ fast path 尚未
完成足够的双线程正确性验证，因此 CPU 会在 `numThreads > 1` 时拒绝该组合，而不是
静默运行一个可能错误的模型。

### 4.1 索引和 tag

PC 先右移一位，以 RISC-V 最小 2B 指令粒度形成 halfword address：

```text
halfword_addr = virtual_PC >> 1
```

- set 数为 2 的幂时，用 mask 取 set、用 shift 取 tag。
- set 数不是 2 的幂时，用 `% setNum` 取 set、用 `/ setNum` 取 tag。

默认总共有 `4 * 384 = 1536` 个 entry。当前每个 entry 实际只保存 **一条** 指令，
因此它不是常见的“一个 entry 保存一串 uop”的 trace/block uop cache。

### 4.1.1 `uopCacheMaxInstBytesPerEntry` 的实际语义

这个参数容易被误解为“一个 cache 存储块最多可以放多少字节，超过后就自动拆分”。
当前代码并不是这样实现的：

1. `UopCache::addInst()` 每次只把当前的一条普通指令写入
   `currentRefillEntry`。
2. 该函数随后调用 `setCurrentUopEntryDone()`，立即把这个单指令 entry 放入
   `refillEntryBuffer`，并新建下一个空 entry。
3. `UopCache::tick()` 再把每个单指令 entry 独立写入 set/way。

因此，在当前实现中，一个 entry 的实际大小只能是：

- 普通 RISC-V 指令：4B；
- compressed 指令：2B；
- macro-op/micro-op：不创建 entry。

不会出现“一个 entry 内指令数超过配置 4B”的运行时场景，也不存在超限后的自动分割、
拒绝写入或溢出告警。`uopCacheMaxInstBytesPerEntry` 当前主要被
`UopCacheStats` 用来表示每个 entry 的**名义容量**，计算 `entryCapacityBytesTotal`
和 `entryByteUtilization`，并设置 `entryBytes` 分布的上界。默认值 4B 下，4B 普通指令
的利用率为 100%，2B compressed 指令的利用率为 50%。

如果未来把 entry 改成真正的多指令 block/trace entry，则必须额外实现容量判断：当累计
字节数加上下一条指令大小超过该参数时，应先结束当前 entry、再开始新的 entry，并明确
分支边界、跨 block 指令、压缩指令和 refill 带宽的处理。目前这些行为尚不存在，不能
根据该参数推断 cache 会自动保存“最多 4B 的多条指令”。

### 4.2 Entry 内容

`UCInstDesc` 保存：

- 已译码的 `StaticInstPtr`
- 指令 PC
- 是否为 16-bit compressed instruction
- 进入 refill buffer 的 cycle

`UopEntry` 还保存 tag、valid、首次指令 PC、覆盖字节数、写入 cache 的 cycle 和上下文。

### 4.3 地址空间上下文

`UopCacheContext` 比较以下 RISC-V 状态：

- `prv`：当前特权级
- `virt`：虚拟化模式
- `satp`：主地址翻译上下文
- `vsatp`：guest 地址翻译上下文
- `hgatp`：hypervisor 地址翻译上下文

这避免相同虚拟 PC 在不同地址空间或特权环境中错误复用同一个译码结果。不过它不是
完整的指令一致性机制，见第 10.8 节。

### 4.4 替换策略和复杂度

替换优先选择空 way，否则使用 per-set tree-PLRU。命中后和 refill 后都会更新 PLRU。

- 单条指令 lookup 会线性扫描所有 way，复杂度为 `O(wayNum)`。
- block lookup 会按 2B/4B 指令长度遍历整个预测 block，每条指令再扫描 way，复杂度为
  `O(blockInstCount * wayNum)`。
- PLRU victim 选择和更新为 `O(log2(wayNum))`。

这些是模拟器宿主机的算法复杂度，当前没有转换成被模拟 CPU 的额外 cycle。

## 5. Refill 路径

cache 由正常路径的 Decode 结果建立：

```text
I-cache/fetch buffer -> decoder -> DynInst -> Decode::decodeInsts()
    -> UopCache::addInst()
    -> refillEntryBuffer
    -> 下一次 Decode::tick() 中 UopCache::tick()
    -> set/way 中的 UopEntry
```

`Decode::tick()` 先调用 `UopCache::tick()`，随后才执行本拍的 `decodeInsts()`。因此本拍
Decode 新加入 refill buffer 的 entry 最早在下一次 Decode tick 落入 cache。该行为表达
了一个 1-cycle 的最小 fill 可见时间，但 `tick()` 会一次写完 refill buffer 中的所有
entry，没有写端口数量限制。

以下情况不会正常向 `UopCache::cache` refill：

- uop cache 被关闭
- 当前处于 stream mode，即正在消费 `UopCache::cache` 中的指令
- 指令本身来自 `UopCache::cache`，防止自我 refill
- 指令是 macro-op 或 micro-op；这两类对象不创建可供本 cache 复用的单条
  `UCInstDesc` entry，仍由普通 decoder 的 macro-op/micro-op 逻辑处理

Decode 根据当前指令来自正常路径还是 uop cache，在 build mode 和 stream mode 之间
切换。squash、部分早期 redirect、taken 分支边界和 `quiesce` 会结束或清理当前 refill
状态。由于当前 entry 只有一条指令，原本面向多指令 entry 的 build/stream 边界语义
已经大幅退化。

## 6. 命中与 I-cache bypass

Fetch 查询分两层：

1. `checkUopCacheBlockHit()` 验证预测 block 内每条 PC 都存在。
2. `checkUopCacheHit()` / `findInst()` 查找当前 PC 对应的 entry 和指令描述。

block 的结束位置为：

- 预测 taken：预测分支 PC 加该分支指令长度。
- 预测 not-taken：FTQ 的 `predEndPC`。

只有 block 完整命中，`checkMemoryNeeds()` 才返回 `NoStall` 并跳过 fetch buffer 字节
检查。`sendNextCacheRequest()` 也会在 block 起始 PC 命中时不发送流水化 I-cache
请求。随后 `processSingleInstruction()` 直接取得保存的 `StaticInstPtr`，并根据
compressed 标记推进 PC 状态。

`PendingUopCacheLookup` 只是在同一 block/PC 的多次函数调用之间复用查询结果。当前
查询、tag/context 比较和 `StaticInstPtr` 读取都发生在同步函数调用中，没有被模拟为
独立的 lookup pipeline stage。

## 7. Decode bypass 与顺序保证

uop-cache hit 不一定成功 bypass。Fetch 首先询问 Decode 的 bypass deque 是否还有
空间。容量由 `uopCacheBypassQueueSize` 参数控制，默认每线程 64 项：

- 有空间：构造的 `DynInst` 不进入普通 `fetchQueue`，而是直接调用
  `enqueueUopCacheBypassInst()`。
- 无空间：仍然复用 uop-cache 的译码结果和 I-cache bypass，但指令回退到普通
  `fetchQueue`，因此不获得固定的 Fetch-to-Decode 延迟收益。

Decode 每拍最多按 `decodeWidth` 把普通路径和 bypass 路径的指令合并进
`fixedbuffer`，再执行已有 Decode 逻辑。合并按 sequence number 排序。CPU 还跟踪最老
的未译码普通指令，`uopCacheBypassOrderBlocked()` 会阻止较新的 bypass 指令越过较老的
普通指令。这样做保证优化只改变到达时间，不改变程序顺序。

squash 时，Decode 会清空相关线程的 bypass queue；每条指令还带 squash version，旧
版本指令在移入 Decode buffer 时会被标记或丢弃。

## 8. Bypass 成功到底节省多少拍

### 8.1 结论

在当前 `kmhv3.py` 和 `idealkmhv3.py` 配置中，相对于“同一 Fetch cycle 已经生成、并在
本拍送入普通 FetchStruct 的指令”，成功 bypass 在无 backpressure、无更老普通指令
阻塞时，**固定节省 2 cycle**。

更一般地，若 `fetchToDecodeDelay = D`，由于当前 stage 调用顺序造成 bypass 至少等待
1 cycle，固定缩短量为：

```text
max(D - 1, 0) cycle
```

当前 `D = 3`，所以为 `3 - 1 = 2 cycle`。

### 8.2 逐拍推导

`CPU::tick()` 的 stage 顺序是：

```text
commit -> iew -> rename -> decode -> fetch -> advance time buffers
```

设 Fetch 在 cycle `F` 创建一条指令：

| Cycle | 普通路径 | bypass 路径 |
| --- | --- | --- |
| `F` | Fetch 写 `toDecode` 的 time-buffer wire | Fetch 在 Decode 已 tick 后写 bypass deque |
| `F+1` | 尚未到 `getWire(-3)` | Decode 从 bypass deque 移入 `fixedbuffer`，可在本拍 decode |
| `F+2` | 尚未到 `getWire(-3)` | 已经离开 Decode |
| `F+3` | Decode 从普通输入移入 `fixedbuffer`，可在本拍 decode | 已经离开 Decode |

因此 bypass 不是“同拍进入 Decode”。它绕过 3-cycle time buffer，但由于 Decode 在
Fetch 之前 tick，仍花 1 cycle 到达 Decode，净节省 2 cycle。

### 8.3 这个数字不包含什么

- **I-cache miss latency**：完整 block 命中可能额外避免若干到很多 cycle，数量随运行
  状态变化。
- **I-cache hit/字节准备时间**：如果普通路径在 cycle `F` 尚不能生成指令，uop cache
  会更早生成 `DynInst`，这也是可变或配置相关收益，不是上述固定 2 cycle。
- **队列等待**：bypass queue 满、Decode blocked、`fixedbuffer` 满或存在更老普通指令
  时，实际节省会小于 2 cycle，甚至只剩 I-cache/decoder 复用收益。
- **后端性能**：早 2 cycle 到达 Decode 不保证程序早 2 cycle 完成，收益可能被后端
  瓶颈覆盖。

### 8.4 建议增加的直接时序统计

当前 `uopCacheBypassInsts` 只数成功从 bypass queue 移到 Decode buffer 的指令，并不
直接记录每条指令节省的拍数。若要用运行数据验证上述静态推导，建议后续为 `DynInst`
记录：

- Fetch 创建 cycle
- 进入 Decode `fixedbuffer` 的 cycle
- 正常路径与 bypass 路径分别统计该差值的 distribution

这样可以区分理论固定延迟和真实队列/顺序阻塞后的有效延迟。

## 9. 统计与结果解释

### 9.1 现有统计

Fetch 侧：

| 统计 | 含义 |
| --- | --- |
| `uopCacheHits` | 在 block 起始 PC 统计的完整 block hit 次数 |
| `uopCacheMisses` | 在 block 起始 PC 统计的 block miss 次数 |
| `uopCacheHitInsts` | Fetch 从 uop cache 取得的动态指令数 |
| `uopCacheBypassQueueFullEvents` | 命中但 bypass deque 满的次数 |
| `uopCacheBypassOrderBlockedEvents` | 因更老普通指令未 decode 而阻塞的检查次数 |
| `uopCacheBypassInsts` | 实际从 bypass deque 移入 Decode buffer 的非 squash 指令数 |
| `uopCachePcMismatches` | 命中后无法按当前 PC 安全供给的异常次数 |

uop cache 自身还记录 entry refill 数、每 entry 指令数/字节数和名义字节利用率。当前每
entry 固定一条指令，因此 `avgEntryInsts` 应接近 1；2B 压缩指令会让 4B 名义容量利用
率低于 100%。

### 9.2 不能直接这样解释

`uopCacheHitInsts` 是推测前端工作量，不是 retired instruction 数。错误路径、squash
后的重新 fetch 和循环重复都会计数，所以它可以超过 `simInsts`。同理，不能只用
`uopCacheHits / (hits + misses)` 就推导整机加速比。

更可靠的 A/B 分析至少应同时比较：

- `simTicks`、IPC 或 benchmark score
- I-cache demand access/miss/stall
- `uopCacheHitInsts`、`uopCacheBypassInsts`
- queue-full 和 order-blocked 事件
- branch misprediction、squashed instructions
- 后端 stall，确认瓶颈是否已经转移

### 9.3 当前 SPEC06 A/B 的方向性证据

已有 CI 对比中，uop cache run `30421280436` 相对 base run `30418078641`：

- `astar` 汇总运行时间约降低 7.04%，score 约提高 7.57%。
- `bzip2` 汇总运行时间约降低 4.62%，score 约提高 4.85%。
- SPEC06 Int score 约提高 2.24%，overall score 约提高 1.40%。

单 checkpoint 中还观察到 I-cache demand access 大幅减少，这与“完整 block 命中后跳过
I-cache 请求”的实现一致。这些数据说明 astar/bzip2 的热点前端路径确实能从当前模型
获益，但不能单独证明真实硬件会获得相同幅度，原因包括下一节的理想化假设，以及两次
CI 必须持续确认除 uop-cache 开关外没有其他配置或代码差异。

## 10. 当前实现可能过于理想的地方

以下限制会让模拟结果偏乐观，或者让它与真实 uop-cache 结构不同。

### 10.1 Lookup 被建模为 0-cycle

block 扫描、所有 way 的 tag/context 比较、PLRU 更新和 decoded instruction 读取都是
同步函数调用。更大容量或高相联 cache 在真实硬件中可能需要一级或多级流水，命中也
未必能在当前 Fetch cycle 直接供给。

### 10.2 没有读端口、bank 和带宽限制

一个 Fetch cycle 内可以查询并读取多条指令，没有 tag/data read port 数量、bank
conflict、跨行读取或多路选择器时序限制。`fetchWidth=32` 时，这个假设尤其激进。

### 10.3 Refill 带宽和读写冲突理想化

refill 至少到下一 Decode tick 才可见，但一次 `tick()` 可以写完所有待填 entry。没有
write port 上限，也没有 lookup 与 refill 同 set/bank 时的冲突。

### 10.4 直接缓存 `StaticInstPtr`

真实硬件需要保存编码后的 uop、操作数描述、控制位、边界信息和纠错元数据，并支付
SRAM 面积与读出能耗。直接保存宿主对象指针既没有物理位宽，也没有面积、功耗或读出
延迟，因此当前 1536-entry 配置不能直接换算成真实硬件容量。

### 10.5 每 entry 仅一条指令

典型 uop cache 常按 line、trace 或 fetch block 保存多条 uop，并处理对齐、跨 line、
分支终止和内部 slot 利用率。当前逐指令 entry 简化了这些问题，同时让 4-way/384-set
的容量和冲突行为不能直接对应某个真实设计。

### 10.6 完整 block 查询本身没有代价

模型为了决定是否跳过 I-cache，会先确认整个预测 block 的每条指令都命中。真实硬件
通常不能在一个 cycle 内串行遍历未知数量、混合 2B/4B 长度的指令后再作决定。当前
模型既拥有译码后长度信息，又不为遍历付出 timing cost。

### 10.7 Context 读取和比较没有代价

每次查询直接读取 `prv/virt/satp/vsatp/hgatp` 并全量比较，没有为寄存器分发、宽 tag
或地址空间标识管理建模。真实实现可能使用更短的 ASID/VMID/tag，也可能在上下文切换
时整体或选择性失效。

### 10.8 指令一致性与失效边界

cache 提供 `invalidateAll()` 和 `invalidateContext()`：二者都会处理已安装 entry 和
尚未安装的 refill 状态，`invalidateAll()` 还会恢复 build mode。Fetch 的统一失效入口
会同时取消所有 per-thread pending lookup，避免复用失效边界之前缓存的 block-hit 结论。

当前接入点包括：

- `fence.i`；
- `sfence.vma`、`sinval.vma` 和现有页表 demap 路径；
- `SATP` 变化触发的 `CPU::flushTLBs()`；
- PMP configuration/address 更新。

因此，遵守 RISC-V `fence.i` 和地址翻译失效合同的软件不会继续命中旧
`StaticInstPtr`。尚未建模的是来自外部 agent 的指令侧 coherence 通知，以及软件未执行
必要 `fence.i` 的非规范自修改代码；这些场景仍超出当前模型保证范围。

### 10.9 I-cache bypass 边界偏乐观

完整 block 命中会直接抑制 I-cache request。真实前端可能仍需完成权限检查、地址翻译、
line/trace 对齐检查或与预测流水对拍。当前 context match 不能代表这些步骤的全部时序。

### 10.10 Bypass 队列只建模容量

每线程 bypass deque 容量由 `uopCacheBypassQueueSize` 参数化，默认 64；模型仍没有为其
面积、读写端口或 bank 冲突建模。大队列会降低 queue-full 概率，从而放大固定 2-cycle
bypass 收益。

### 10.11 错误路径也享受优化

模型会为预测路径查询 cache、创建指令和计 hit，即使这些指令后来被 squash。真实硬件
当然也会推测执行，但错误路径的查询带宽、能耗、污染和恢复时序没有被完整约束。因此
命中指令数和被避免的 I-cache access 可能显得非常大。

### 10.12 替换与容量缺少物理校准

tree-PLRU 本身合理，但其读取/更新没有 timing cost；默认 set/way/entry 数也尚未由某个
RTL SRAM 宏、面积预算或目标处理器参数校准。结果更适合做机制上界探索，而非面积等价
的产品预测。

## 11. 推荐验证方法

### 11.1 最小功能 A/B

保持 binary、checkpoint、随机种子和所有 CPU 参数不变，只切换：

```text
# on
--enable-uop-cache

# off
<不传 --enable-uop-cache>
```

先验证最终 committed instruction 数和架构结果一致，再比较周期和前端统计。

### 11.2 时序验证

用 `--debug-flags=UC,Decode,Fetch` 选择一个短小循环，检查同一 sequence number：

1. Fetch 在 cycle `F` 打印 enqueue bypass。
2. Decode 在 `F+1` 打印 moved to decode buffer。
3. 普通路径对照在 `F+3` 从 FetchStruct 到达。

为了做批量验证，优先实现第 8.4 节的 latency distribution，而不是解析大量日志。

### 11.3 性能归因

建议至少选择三类 workload：

- 小热点循环：验证高命中和固定 bypass 延迟收益。
- I-cache working set 较大：观察容量/冲突和 I-cache miss avoidance。
- 后端受限 workload：验证高命中但低 IPC 收益的反例。

逐步扫 `setNum`、`wayNum` 和一个未来可参数化的 lookup latency，可以得到容量、命中率
和时序的敏感性曲线。若目标是与 RTL 对齐，还必须加入真实 entry 格式、bank/port、
lookup stage、refill 带宽和失效协议后再比较。

## 12. 代码导航

- `src/cpu/o3/uop_cache.hh`：entry、context、cache 接口和 stats 声明。
- `src/cpu/o3/uop_cache.cc`：索引、lookup、PLRU、refill 和 invalidation 实现。
- `src/cpu/o3/fetch.cc`：block/PC 查询、I-cache bypass、DynInst 构造和命中统计。
- `src/cpu/o3/decode.cc`：refill、bypass deque、顺序合并和 squash 清理。
- `src/cpu/o3/cpu.cc`：stage tick 顺序和 Fetch/Decode bypass 转发接口。
- `src/cpu/o3/dyn_inst.hh`：`fetchFromUopCache`、`uopCacheBypass` 等动态标记。
- `src/cpu/o3/BaseO3CPU.py`：通用参数默认值。
- `configs/common/Options.py`：`--enable-uop-cache` 命令行开关。
- `configs/example/kmhv3.py`、`configs/example/idealkmhv3.py`：Kunminghu v3 配置。

## 13. 总结

当前实现把“缓存已译码指令”和“绕过普通 Fetch-to-Decode 延迟”组合在一起。对热点
代码，它能够同时减少 I-cache/decoder 工作，并在 bypass 成功时把 Fetch 创建到 Decode
消费的最短距离从 3 cycle 降到 1 cycle，固定净省 2 cycle；如果还避免了 I-cache miss，
总收益可以更大但不固定。

这个模型很适合探索 uop cache 的潜在上界和识别前端敏感 workload。解释 astar、bzip2
等明显加速时也必须保留边界：0-cycle lookup、无限读写带宽、逐指令 entry、直接缓存
`StaticInstPtr` 和大 bypass deque 都会使结果比可实现硬件更理想。下一
阶段若要把性能数字用于硬件决策，最重要的工作是加入可参数化 lookup latency 与端口/
bank 限制，并按目标 RTL 的 entry 格式和一致性规则重新校准容量。
