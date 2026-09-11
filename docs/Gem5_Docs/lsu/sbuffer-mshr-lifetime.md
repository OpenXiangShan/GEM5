# SBuffer 表项与 MSHR 生命周期

## 1. 核对结论

原实现确实把 **SBuffer 物理表项的释放绑定到了最终缓存响应**，而不是 store miss 被 MSHR 接收的时刻：

1. `LSQ::issueSbufferPacketFromDcacheMainPipe()` 在 fake MainPipe S2 调用 classic L1D 的 `sendTimingReq()`。
2. 请求成功且为 miss 时，退出 fake MainPipe，并设置 `entry->sending = true`，但不归还 SBuffer 表项。
3. classic cache 等待下级响应，执行 `serviceMSHRTargets()`，应用写数据并回复原请求。
4. `SbufferRequest::recvTimingResp()` 调用 `LSQ::completeSbufferEvict()`，后者才执行 `storeBuffer.release()`。

因此原模型中，长延迟 store miss 会长时间同时占用 SBuffer 和 MSHR。严格说，释放条件是“最终响应到达 LSQ”，并非直接监听 MSHR deallocate；fake refill MainPipe 还可能在 classic MSHR 释放后继续持有 MSHR credit。hit 路径不涉及 miss MSHR，也不能一概描述为等待 MSHR 释放。

### RTL 对照

核对本地 `XS-KMHV3-260826`，commit `f91bdbb8c69e2ad9e041a7dcd92146f5a397d61c`：

- `src/main/scala/xiangshan/cache/dcache/mainpipe/MainPipe.scala`：`mshr_handled_store_miss` 表示被 MSHR 接收且没有 replay 的 store miss；寄存到下一拍后，通过 `store_hit_resp` 返回 ACK，并设置 `miss` 标志。
- `src/main/scala/xiangshan/mem/sbuffer/Sbuffer.scala`：`hit_resps.fire` 清除对应表项的 valid/inflight；这条 ACK 不等同于写数据已经回填。
- 同一文件中，`sbuffer_mshr_empty = sbuffer_empty && io.mshr_store_empty`，说明 flush 排空仍需等待 MSHR 中的 store。
- `MissQueue.scala` 的 store merge 条件不是任意 store-to-store 合并；MSHR 同时承担已接收 store 的数据保存与 store-to-load forwarding。

这里的 RTL 对照仅针对上述本地版本，不声称所有 RTL 分支实现都相同。

## 2. 建模合同

核心因果链为：

```text
已提交 store → SBuffer 分配/合并 → MainPipe S0/S1/S2
    → L1D hit：沿用原响应路径，响应时释放表项
    → miss 被接受：释放 SBuffer 容量，保留在途写请求
        → 下级响应/应用写数据 → 完成可见性更新与在途请求清理
    → 请求被拒绝：保留表项与数据，进入原 S0 replay 路径
```

必须分开以下三种事件：

| 事件 | 释放 SBuffer 容量 | 数据已全局可见 | 在途请求可销毁 |
| --- | --- | --- | --- |
| 进入 fake MainPipe | 否 | 否 | 否 |
| S2 miss 被 L1D/MSHR 接收 | 是 | 不保证 | 否 |
| 最终写响应 | 已提前释放；hit/旧模式在此释放 | 沿用原模型的可见性时点 | 是 |

MSHR 满、target 满、分配仲裁失败、alias/write-buffer 冲突及缓存端口拒绝都不能触发表项释放。判断依据是实际 timing handshake 成功且为 miss，而不是“尝试发送”。

## 3. 实现与正确性边界

### 独立保存写数据

`SbufferRequest::releasedEntry` 在 miss 被接受时保存一份稳定的表项快照，保留地址、线程、序号、字节 mask 和写数据。请求及 Packet 的数据指针改指向该快照，然后归还原物理表项。快照不是另一个 SBuffer 容量 token。

这一步是必要的：原 Packet 使用 `dataStatic()` 指向表项内存，若只提前调用 `release()`，新 store 复用表项就会修改尚未完成的旧请求数据。最终响应也不能再次释放已被复用的物理槽位。

### Forwarding 与同地址写顺序

`sbufferMissRequests` 按物理 cache line 索引被接受的 store miss，模拟 MSHR 中保存的 store forwarding 数据。只向同线程且更年轻的 load 转发，逐字节优先级为：

```text
live SBuffer vice → live SBuffer entry → 已接收 miss 的快照
```

它支持全覆盖转发，以及新 store 只覆盖部分字节时与旧 miss 数据组合；不把旧 miss 的字节覆盖到更年轻的 store 上，也不跨 SMT 线程直接转发。

这里复用原有 SBuffer forwarding 完成路径保存 miss 数据的可转发性，没有新增 RTL MissQueue forwarding 的独立查询端口和拍级延迟。本次校正的是容量寿命，并非完整重建 MSHR forwarding 流水线。

同一物理行最多保留一个尚未完成的 SBuffer miss。后续同地址 store 在 S2 replay，等待前一个请求最终响应后再发送。这是有意的保守约束：不新增 RTL 没有承诺的任意 store-to-store MSHR 合并，也避免经典缓存 write target 排序反转造成旧写覆盖新写。不同物理行仍可并行占用不同 MSHR，不再被已接收请求的 SBuffer 表项限制。

### Flush、fence、drain 和可见性

- `storeBuffer.full()`、`storeBuffer.size()` 和容量统计只计仍占用的物理表项。
- `LSQ::storeBufferEmpty()` 作为现有排空接口，要求物理表项与已接收的 store miss 都为空；per-thread 和按序号的版本也保持这一语义。
- 每线程 `sbufferMissSeqs` 保存未完成 miss 的序号，fence/flush 只等待相应线程、相应序号范围内的写。
- `LSQ::isDrained()` 同样不能忽略这些写请求。
- golden memory 更新、跨线程 store-visible 通知和相关 replay 处理仍发生在最终响应时，不提前到 MSHR 接收时。

MSHR 和 fake refill credit 原有的分配/释放时点不变；本修改只分离 SBuffer 容量与在途写请求的生命周期。

## 4. 参数、复杂度与统计

`src/cpu/o3/BaseO3CPU.py` 增加 `sbufferReleaseOnMiss`，默认 `True`，适用于使用该 LSQ 的 O3 配置，包括 `kmhv3.py`。设置 `False` 可对照旧的“等最终响应才释放”行为：

```text
--param 'system.cpu[0].sbufferReleaseOnMiss=False'
```

本修改不调整 `SbufferEntries`、`SbufferEvictThreshold`、MSHR 数量或端口带宽。默认开启会改变 store-miss 较重 workload 的 SBuffer 占用和阻塞趋势，这是预期行为。

- miss 地址索引平均 O(1)；同地址重试和转发查询不扫描整个 MSHR 集合。
- 每个被接受的 miss 复制一条 cache line，成本 O(cache-line bytes)。
- 每线程序号 multiset 插入/删除为 O(log M)，查看最早未完成序号为 O(1)。
- 每行最多一个快照；数量受 MSHR、接收带宽以及最终响应排队/延迟共同约束。由于 classic MSHR 释放和 CPU 收到响应可能不同拍，快照数量不应被硬性断言为不超过 MSHR 配置值。
- 不逐信号复制 RTL：RTL S2 接收后下一拍 ACK，本模型在现有 S2 回调中完成容量移交，省略单独 ACK 寄存器，保留资源寿命差异和所有失败重试边界。

新增统计位于 LSQ stats group：

| 名称 | 含义 |
| --- | --- |
| `sbufferMissEntriesReleased` | miss 接收时提前释放的表项数 |
| `sbufferMissPending` | 已释放表项但仍待最终响应的 store miss 平均数量 |
| `sbufferMissSameLineReplay` | 因同物理行已有未完成 store 而被拒绝的 S2 尝试数 |
| `sbufferMissForward` | 使用已接收 miss 数据的 load forwarding 查询数 |

结合原有 `sbufferAvgEntryNum`、`sbufferFullCycles`、`sbufferDcacheReqBlocked`、`dcacheMainPipeStoreS2MissExit` 观察瓶颈从 SBuffer 容量转向 MSHR/端口竞争。`StoreBuffer` debug flag 会输出提前释放时点、原请求的最终响应和物理占用。

## 5. 定向回归

`util/xs_scripts/sbuffer_release/run.py` 使用自检裸机汇编，比较旧模式、新模式和少 MSHR 压力场景。测试以单表项 SBuffer 强迫槽位复用，覆盖多行并行 miss、旧写数据稳定性、全量/部分字节转发、同地址写顺序以及 fence 后的数据校验。先运行一遍相同代码，再换新的数据地址，以减少冷 ICache 对定向 miss 场景的干扰。

```sh
scons build/RISCV/gem5.opt --gold-linker -j16
python3 util/xs_scripts/sbuffer_release/run.py \
  --gem5 build/RISCV/gem5.opt \
  --outdir /tmp/sbuffer-release-regression
```

要求 PATH 中存在 `riscv64-linux-gnu-gcc` 和 `riscv64-linux-gnu-objcopy`。测试使用普通 raw bin，不需要外部 GCPT restorer 或 AM_HOME；默认禁用 difftest，依靠程序自检及日志/stats 断言。可用 `--ref-so <path>` 开启 NEMU difftest，运行前需确认 reference 可用。

每组输出 `command.json`、`sim.log`、`store.trace`、`stats.txt`；整体通过后输出 `results.json`。仅编译通过不能证明生命周期正确，至少还应确认提前释放发生在最终响应之前，且一个 SBuffer 槽位能支撑多个同时在途的不同 cache line miss。

### 本地实测：2026-09-09

使用 `kmhv3.py`、1 个 SBuffer 表项、驱逐阈值 0、SimpleMemory 延迟 500 ns。三组均通过裸机数据自检、日志时序断言；开启 NEMU difftest 后重复三组也全部通过，关键统计一致。

| 指标 | 旧模式 / 16 MSHR | 新模式 / 16 MSHR | 新模式 / 2 MSHR |
| --- | ---: | ---: | ---: |
| 提前释放次数 | 0 | 16 | 16 |
| 已释放槽位的最大在途 miss 数 | 0 | 8 | 2 |
| SBuffer 平均物理占用 | 0.665096 | 0.060027 | 0.350657 |
| miss 数据转发查询数 | 0 | 2 | 1 |
| 同行 store S2 replay 次数 | 0 | 789 | 0 |
| L1D 无 MSHR 阻塞周期 | 0 | 0 | 12830 |
| CPU cycles | 38615 | 27472 | 32212 |

例如新模式的第一条 store miss：tick `1088577` 已释放地址 `0x80010000` 的 SBuffer 表项，tick `1622043` 才收到该请求的最终响应；其间原物理槽位又被用于另外 7 个 cache line。这直接证明表项释放不再依赖回填完成，同时 NEMU 对拍验证了槽位复用没有破坏旧请求数据。

检查通过：

- `scons build/RISCV/gem5.opt --gold-linker -j16`。
- `python3 util/style.py -m src/cpu/o3/lsq.cc src/cpu/o3/lsq.hh` 与回归脚本的 repository style check。
- `git diff --check`。
- 不启用 difftest 的三组回归：`/tmp/xs-sbuffer-release-regression/results.json`。
- 使用 `/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so` 的三组回归：`/tmp/xs-sbuffer-release-difftest/results.json`。

这些结果仅用于证明本修改的资源生命周期、转发和重试因果关系，不代表 SPEC 收益。尚未执行完整 SPEC checkpoint 回归、SMT 多线程运行或 drain/checkpoint 专项测试；SMT 隔离与 drain 条件已做源码检查，但不能把这部分视为已完成运行验证。
