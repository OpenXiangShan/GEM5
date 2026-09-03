# Partial Store 权限请求与部分有效 Cacheline 设计

## 1. 背景与目标

当前 classic cache 在 partial store miss 时发送 `ReadExReq`，同时取得写权限和完整 cacheline 数据。目标是在单核、无多核 snoop 的场景中，将该路径改为只申请权限，避免不必要的下层和 DDR 读流量。单核内 DTB walker 等 coherent client 发出的 shared read 仍需正确处理。

当前实现只覆盖 classic timing L1D 的普通 cacheable StoreBuffer 写，不支持 Ruby、多核 snoop、AMO、LL/SC、uncacheable、压缩 cache 或 DMA coherence。

## 2. 建模合同

性能因果链为：

```text
partial store miss
    -> permission-only MSHR transaction
    -> store 提前完成且不读取 cacheline
    -> 后续未覆盖 load 延迟补全数据，或驱逐时 masked writeback
    -> 改变 MSHR、互连、读写队列和 DDR 流量
```

权限请求复用现有 tag、MSHR、互连和响应资源，不增加独立延迟参数。未覆盖 load 仍承担正常 cacheline read 延迟；partial eviction 仍占用 Write Queue 和下层写带宽。

## 3. L1D 状态模型

在 line-level MOESI 状态之外，为 L1D block 增加部分数据有效状态：

| 状态 | Tag | Writable | Dirty | 数据语义 |
| --- | --- | --- | --- | --- |
| `I` | 无效 | 否 | 否 | 无数据 |
| `PartialModified` | 有效 | 是 | 是 | 仅 `validMask` 覆盖的字节有效 |
| 普通 `M` | 有效 | 是 | 是 | 整行有效 |

`CacheBlk` 增加按需分配的 `validMask`。普通 block 不分配 mask，隐含整行有效；只有 `PartialModified` block 保存 mask。主要转移如下：

```text
I + partial store -> PermissionPending -> PartialModified
PartialModified + store -> 扩展 validMask
PartialModified + covered load -> hit
PartialModified + uncovered load -> DataFillPending -> M
PartialModified + eviction -> masked WritebackDirty
```

当 `validMask` 变为全满时，block 立即退化为普通 `M`，清除 partial 元数据并回到现有路径。

## 4. Permission-Only 请求

新增 `StorePermReq` 和 `StorePermResp`。`StorePermReq` 具有 upgrade/invalidate、needs-writable、needs-response 和 from-cache 属性，但没有 read 属性和数据 payload。

L1D 仅在 block invalid 且 StoreBuffer 写未覆盖整行时生成 `StorePermReq`，代替 `ReadExReq`。已有完整 S/E/M block 上的写和 full-line write 保持现有行为。

下层 cache 按以下规则处理：

- writable hit：tag-only 完成并返回 `StorePermResp`；
- read-only hit：继续向下申请 writable；
- miss：不分配 block，继续转发；
- 所有 cache miss：内存控制器返回无数据响应，不进入 DRAM 读写队列。

普通 `UpgradeReq` 假定上层已有完整数据，因此不能直接复用其 dirty ownership 转移语义。`StorePermReq` 命中下层 Dirty block 时必须保留下层 Dirty 和完整数据，不能清 Dirty 或宣称整行数据已经转移给 L1。

## 5. MSHR 与并发请求

MSHR 增加 `MissKind`：`Normal`、`WholeLineWrite`、`PartialPermission`、`PartialDataFill` 和 `PartialSnoopFill`。

`PartialPermission` 未完成时，后续 store 可以进入 active targets 并合并 byte mask；load 必须进入 deferred targets。`StorePermResp` 只服务 store targets。随后 deferred load 重新检查 L1 mask：已覆盖则命中，否则发起 `PartialDataFill`。

`PartialDataFill` 使用普通整行 read 请求取得下层数据，但 refill 只能复制 `~validMask` 对应的字节。响应期间到达的 store 先更新 block；refill 必须再次读取当前 mask，不能用请求发出时的旧 mask 覆盖新数据。

补齐请求分配或从 deferred target 提升时，MSHR 会立即标记为 partial fill。若其他 refill 的 replacement victim 命中该 MSHR，缓存沿用现有冲突处理：拒绝替换 partial block，并用 `tempBlock` 完成其他 refill。`partialFillVictimConflicts` 统计这种被阻止的替换尝试。

## 6. Shared Snoop 补全

DTB walker 等单核 coherent client 的 `ReadSharedReq` 可能经 CoherentXBar snoop L1D。若命中 `PartialModified`，L1D 不能直接返回不完整数据，也不能让请求继续访问下层后与 L1D 的新数据失去一致性。此时 L1D 成为 ordering point：

```text
ReadSharedReq snoop -> reserved MSHR -> internal full-line ReadSharedReq
                    -> merge invalid bytes -> deferred snoop response
```

原 snoop 被标记为由 L1D 响应；其副本作为 `FromSnoop` target 等待补全。内部请求复制 `Request`，避免与 CoherentXBar 中原事务的路由身份冲突。若同地址已有 read MSHR，则只追加 snoop target，并复用在途 fill。

普通请求看不到 `partial_snoop_mshr_reserve` 提供的应急 MSHR，默认保留 4 项，避免不可重试的 snoop 因普通 MSHR 满而丢失。保留项耗尽会显式 panic；该恢复路径、atomic snoop 和多核竞争不在当前模型范围内。

## 7. Masked Writeback

Partial line eviction 生成 cacheline 大小的 `WritebackDirty`，携带数据和 `byteEnable=validMask`：

- 下层命中完整 line：只覆盖 enabled bytes，置 Dirty 并消费 packet；
- 下层 miss：不调用 `allocateBlock()`，将 packet 放入本层 Write Queue 后原样下传；
- 所有 cache 都 miss：最终作为一次 masked write 到达内存，只更新 enabled bytes。

这不是物理绕过 L2。packet 始终沿 L1 Write Queue、互连、L2 Write Queue和内存端口逐级传递，只 bypass 下层 miss allocation 和 `ReadExReq`。

同地址 Write Queue 必须保持新旧数据顺序：full + partial 时把 partial overlay 到 full packet；partial + partial 时数据按新请求覆盖且 mask 做 OR；partial + full 时由 full packet 取代旧 partial packet。若同地址 MSHR 或已发送 writeback 存在，则沿现有 order/retry 机制串行化，禁止旧整行写回晚于新 partial 数据生效。

## 8. 代码落点

- `src/mem/packet.hh`、`packet.cc`：新增命令，令 masked write 判断覆盖 `WritebackDirty`。
- `src/mem/cache/cache_blk.hh`：partial 状态、valid mask 和覆盖判断。
- `src/mem/cache/cache.cc`、`base.cc`：miss 分类、partial hit、选择性 refill、masked eviction 和下层 bypass。
- `src/mem/cache/mshr.hh`、`mshr.cc`：`MissKind`、snoop target、target 延迟和 store mask 合并。
- `src/mem/cache/base.cc`：同地址 masked writeback 合并。
- `src/mem/packet.cc` 和 functional cache 路径：按 byte mask 组合 functional data，避免 queued partial writeback 被忽略。
- `src/mem/abstract_mem.cc`、`mem_ctrl.cc` 和 `simple_mem.cc`：权限请求终止和 functional 数据合成。
- `src/mem/cache/Cache.py`：增加 `enable_partial_store` 和 `partial_snoop_mshr_reserve`；仅在 `configs/example/kmhv3.py` 的 L1D 显式开启 partial store。

启用时应检查单核、L1D 和非压缩 cache。下层 eviction 对 partial block
发起的 presence-only snoop 只设置 `BLOCK_CACHED`，不提供数据或改变 block
状态；shared read snoop 走补全路径，其他 partial block snoop 仍显式报错，防止超出模型边界后静默返回无效数据。

## 9. 统计与验证

新增 `partialPermissionReqs`、`partialPermissionLatency`、`partialDataFillReqs`、`partialReadMisses`、`partialCoveredLoadHits`、`partialLineWritebacks`、`partialWritebackBytes`、`partialWritebackMerges`、`partialWritebackBypasses`、`partialSnoopFills`、`partialSnoopMerges`、`partialSnoopFillLatency` 和 `partialSnoopReserveFull`。其中 `partialReadMisses` 仅统计 L1D 收到的 `ReadReq` 访问 partial-valid cacheline 且请求范围未被 valid mask 覆盖的次数；snoop latency 统计从捕获 snoop 到补全数据可用的 tick，不含最终 snoop response 在互连上的返回时间。

验证覆盖以下情形：

1. L2 clean hit、dirty hit和完全 miss 的 permission 路径；
2. covered load 不访问下层，uncovered load 补全后保留 store 数据；
3. 多个 partial store 合并成完整 line；
4. L1/L2 驱逐后逐字节检查最终内存；
5. 同地址 Write Queue 冲突和 permission MSHR 期间的 load/store 合并；
6. `enable_partial_store=False/True` A/B 功能结果一致；
7. 开启后 `ReadExReq`、`bytesReadSys` 和 StoreBuffer DDR read 减少，延迟补全与 masked write 流量能由新增统计解释。
8. DTB walker shared read 命中 partial block 时正确补全，且不产生 masked eviction writeback。

基础构建命令为：

```bash
scons build/RISCV/gem5.opt --gold-linker -j64
```

聚焦单测为：

```bash
scons build/RISCV/mem/cache/partial_store.test.opt --unit-test -j16
build/RISCV/mem/cache/partial_store.test.opt
```

`tests/test-progs/partial-store/partial_store.c` 提供端到端 workload：跨越 L1 容量随机写每行一个字节，再检查写入字节与相邻未写字节。它用于 A/B 检查 permission request、延迟补全、masked eviction、逐级 bypass 和最终内存数据。

实现验收不能只检查 store 提前返回，还必须证明补全、驱逐、functional access 和同地址写回顺序均保持数据正确。

当前定向 workload 的开关 A/B 均输出 `partial-store: PASS`。统计重置后的
partial run 产生 2056 次 permission request、47 次 data fill、107 次 covered
load 和 2023 次 masked writeback；CPU demand memory read 从基线 139456 B
降至 21824 B。改变 demand 时序可能诱发不同的 prefetch 流量，因此总 memory
read 不作为该机制的单一验收指标，应结合 requestor 分类和上述 partial stats
归因。内存控制器的普通 bytes-written 统计仍按 packet size 计数，不等同于
`partialWritebackBytes` 记录的实际 enabled bytes。

性能 A/B 使用 `tests/test-progs/partial-store-perf/`。它通过 nexus-am 构建裸机
镜像，在跨 cacheline 的单字节 cold-store 区间前后读取 `mcycle/minstret`，并用
`fence` 将 StoreBuffer drain 纳入测量。以下命令构建同一个 workload，分别用
`--disable-partial-store` 和 `--enable-partial-store` 运行，再汇总 ROI cycle 和
关键内存统计：

```bash
python3 util/partial_store_perf.py --am-home ../nexus-am
```
