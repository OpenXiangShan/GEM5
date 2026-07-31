# XiangShan RTL 与 gem5 的 DTLB 实现对比

## 1. 对比范围

本文比较 XiangShan RTL、修改前的 gem5，以及修改后的 gem5 两种可配置 DTLB 模式。

重点不是罗列多个 mismatch，而是说明不同模式如何组织 data TLB，以及当前 gem5 做了哪些对齐、保留了哪些近似。

核心问题是：RTL 将 load 和 store 的 L1 地址翻译状态分开；修改前的 gem5 让 load 和 store 共享一个 L1 `dtb`。修改后的 gem5 保留共享模式作为默认行为，同时提供显式开关启用独立 `stb`，用于对齐 RTL 的 L1 资源所有权。

## 2. XiangShan RTL 如何处理 DTLB

### 2.1 两个独立的 L1 TLB

RTL 在 `XiangShan/src/main/scala/xiangshan/mem/MemBlock.scala:581-619` 分别实例化 load 和 store 的 `TLBNonBlock`：

```scala
dtlb_ld_tlb_ld = new TLBNonBlock(..., ldtlbParams)
dtlb_st_tlb_st = new TLBNonBlock(..., sttlbParams)
```

`Parameters.scala:228-245` 将 `ldtlb` 和 `sttlb` 都设置为 `NWays = 48`；`MMUConst.scala:27-41` 默认 `NSets = 1`、`Associative = "fa"`。所以 RTL 可以简化理解为：

```text
load  -> ldtlb, 48 entries
store -> sttlb, 48 entries
```

两个 TLB 拥有各自的 entry 和命中状态。load entry 不会因为 store 填入新页而被逐出，store entry 也不会反向逐出 load entry。

### 2.2 每个 TLB 独立判断 hit/miss

`TLB.scala:542-587` 的 `handle_nonblock()` 中，每个 TLB requestor 根据自己的 `missVec` 判断是否命中、是否等待翻译、是否 replay，以及是否请求下一级翻译。

因此，load 已经翻译过某个 VPN，只说明 `ldtlb` 可能命中；store 访问同一个 VPN 时，仍会在 `sttlb` 中独立查找。

`MemBlock.scala:660-700` 又根据 response vector 选择 refill 目标：

```text
load translation response  -> refill ldtlb
store translation response -> refill sttlb
```

load 的翻译完成不会顺便把同一个 VPN 写入 `sttlb`。

## 3. 修改前的 gem5 如何处理 DTLB

### 3.1 一个共享的 data TLB

修改前 `src/arch/riscv/RiscvMMU.py` 的拓扑是：

```text
itb -> l2_shared
dtb -> l2_shared
```

所有 data read 和 data write 都先进入同一个 `dtb`：

```text
read  -> dtb, 48 entries
write -> dtb, 48 entries
```

修改前没有独立的 `stb`，因此 48 个 data L1 entry 由 load 和 store 共同使用。

### 3.2 read/write 复用同一个 entry

修改前的同一页行为是：

```text
load A  -> dtb miss -> refill dtb
store A -> dtb hit
```

只要权限和地址空间没有变化，load 建立的 entry 对后续 store 也可见。read/write 不需要选择不同的 L1 TLB。

### 3.3 修改前 store miss 的处理

修改前 `LSQUnit::storeDoTranslate()` 调用 `inst->initiateAcc()` 进行 TLB access；如果翻译被延迟，就设置 `TLBMissReplay`，见 `lsq_unit.cc:1819-1834`。后续 `needTLBMissReplay()` 会让 IEW defer 该指令，见 `lsq_unit.cc:1972-1980`。

这条 replay 仍然回到唯一的 `dtb`，而不是进入独立的 store TLB。

## 4. 修改后的 gem5 如何提供两种模式

### 4.1 默认保持一个共享 dtb

`src/arch/riscv/RiscvMMU.py` 增加 `enable_store_tlb` 参数，默认值为 `False`。默认模式下，load 和 store 仍然进入原来的 `dtb`：

```text
enable_store_tlb = false

read  -> dtb, 48 entries
write -> dtb, 48 entries
stb   -> 不接收翻译请求
```

因此，不指定新开关时，修改后的 gem5 保留修改前的关键行为：load refill 得到的 entry 可以被同页 store 复用，load 和 store 继续竞争同一组 48 个 L1 data TLB entry。

`stb` 对象仍然存在于静态 SimObject 配置图中。这是因为 gem5 的端口和 SimObject 拓扑在配置阶段建立，不能在运行过程中动态增加 TLB 和 walker。但默认模式不会把 translation、flush、demap、权限切换或 CPU takeover 操作路由给 `stb`，所以它不是有效的第二套 data translation 状态。

### 4.2 显式开启独立 store TLB

KMHv3 配置提供 `--enable-store-tlb`：

```bash
build/RISCV/gem5.opt configs/example/kmhv3.py --enable-store-tlb ...
```

开启后，原来的 `dtb` 作为 load TLB，新增的 `stb` 作为 store TLB：

```text
enable_store_tlb = true

read  -> dtb, 48 entries
write -> stb, 48 entries
```

`dtb` 和 `stb` 的 `next_level` 都指向同一个 `l2_shared`。这对应 RTL 的主要结构决策：load/store 的 L1 entry 和命中状态分离，但二级翻译状态仍然共享。

### 4.3 按参数和 translation mode 统一路由

`src/arch/riscv/mmu.hh` 中的 `dataTlb(mode)` 同时检查 `enableStoreTlb` 和访问模式：

```text
enableStoreTlb && mode == Write -> stb
其他 data translation           -> dtb
```

timing、atomic、functional 和 `finalizePhysical` 入口都使用这条规则。

这一步的目的不是只修改 load/store 的主 timing 路径，而是避免出现“执行时走 stb、functional 或 permission path 又走回 dtb”的不一致。

### 4.4 只在开启时维护 stb 生命周期

generic MMU 只从 ITB/DTB 根遍历 TLB 层级。`src/arch/riscv/mmu.hh` 因此在 `enableStoreTlb` 开启时，显式把 `stb` 加入：

- `flushAll()`；
- `demapPage()`；
- `setOldPriv()` / `useNewPriv()`；
- `takeOverFrom()` 调用路径。

这样 split 模式下的 `SFENCE`、页失效和权限切换不会只清理 load TLB 而遗留 store TLB entry；shared 模式则不会无意义地操作未使用的 `stb`。

### 4.5 让两个 walker 共享下游 data walker port

`RiscvMMU.py:53-78` 新增 `NoncoherentXBar`：

```text
dtb walker --+
             +--> data_walker_xbar --> 原 data walker cache/port
stb walker --+
```

这保留了一个重要的性能约束：两个 L1 TLB 的 page-table memory request 最终仍会在同一个下游端口相遇，而不是各自连接一套完全独立的 data memory path。

### 4.6 修复共享 timing port 的 retry 状态

增加第二个 walker 后，一个 walker port 可能同时被多个 `WalkerState` 使用。`pagetable_walker.hh:85-99,450-469` 增加 walker 级 `portBlocked`，`pagetable_walker.cc:499-539` 使请求遵守：

```text
sendTimingReq() 被拒绝
  -> walker 进入 blocked
  -> 等待 recvReqRetry()
  -> 再尝试等待中的 state
```

这修复的是 gem5 timing port 协议问题，同时保证新增的 stb walker 不会绕过下游 port 的 backpressure。

### 4.7 配置参数同步

`configs/common/xiangshan.py` 定义默认关闭的 `--enable-store-tlb`。`configs/example/kmhv3.py` 和 `configs/example/idealkmhv3.py` 将其写入每个 CPU 的 `cpu.mmu.enable_store_tlb`；只有开关开启时，才把 L1 direct compression、PTW level limit、各级并行度和 PTW miss queue size 同步应用到 `stb`。这样默认运行保持共享模式，split 模式中的 store TLB 又不会因为缺少配置而拥有与原 `dtb` 不同的资源限制。

### 4.8 对齐结果和保留的近似

开启 `--enable-store-tlb` 后，gem5 对齐了 RTL 最重要的 L1 资源所有权：

- load/store 分别拥有独立的 48-entry L1 TLB；
- write translation 稳定路由到 `stb`；
- 两个 L1 共享 `l2_shared` 内容状态；
- 两个 walker 的下游 memory port 重新汇合。

但它不是 RTL 的逐周期复刻：两个 gem5 walker 仍各自拥有 walker state 和 PTW quota。因此 split 模式的定位是“修正 L1 load/store 所有权并保留主要下游约束”，不是完整复制 RTL 的全部 DTLB 控制状态。

默认 shared 模式的目标不同：它优先保持历史 gem5 行为和性能基线，不宣称与 RTL 的 load/store L1 所有权对齐。

## 5. 分离和共享两种组织方式的优势与代价

### 5.1 分开 load/store TLB 的优势

假设 load 工作集有 40 页，store 工作集有另外 40 页：

- RTL 可以让 40 个 load entry 留在 `ldtlb`，40 个 store entry 留在 `sttlb`；
- load 和 store 不会因为共享一个 48-entry 结构而互相逐出。

这更接近硬件中 load/store 翻译资源分离的结构，也能降低 mixed workload 中的跨类型容量污染。

### 5.2 分开 load/store TLB 的代价

假设初始时 VPN `A` 不在任何 TLB：

```text
load A  -> ldtlb miss -> refill ldtlb
store A -> sttlb lookup
```

即使 load 已经得到 `A` 的物理页号，store 仍要在 `sttlb` 中重新查找：

- 如果 `sttlb` 已经有 `A`，store 命中；
- 如果没有，store 产生自己的 miss，等待翻译完成并 replay。

所以 RTL 可能比单一 `dtb` 多一次 store-side L1 lookup，甚至多一次 store-side miss/replay。

### 5.3 共享 dtb 的优势

同一 VPN 的 load 和 store 可以共享一个 entry，避免重复的 L1 lookup 和 store-side refill。对于 load/store 地址高度复用的程序，这种方式简单且容易命中。

### 5.4 共享 dtb 的代价

如果 load 和 store 访问不同页面、总工作集超过 48 页：

```text
load pages + store pages -> 共同竞争 dtb 的 48 entries
```

store 新页可能逐出仍有用的 load entry，load 新页也可能逐出 store entry。修改前的 gem5 可能因此产生更多 L1 TLB miss 和 replay，无法反映 RTL 的独立容量。

## 6. 同页 load/store 的直接对照

| 阶段 | RTL | 修改前 gem5 | 修改后 gem5，默认 | 修改后 gem5，开启 split |
| --- | --- | --- | --- | --- |
| 第一次 load | `ldtlb` miss，完成后写入 `ldtlb` | `dtb` miss，完成后写入 `dtb` | `dtb` miss，完成后写入 `dtb` | `dtb` miss，完成后写入 `dtb` |
| 随后的 store | 独立查询 `sttlb`，可能 miss/replay | 查询同一个 `dtb`，通常直接 hit | 查询同一个 `dtb`，通常直接 hit | 独立查询 `stb`，可能 miss/replay |
| 后续 load | 查询 `ldtlb` | 查询 `dtb` | 查询 `dtb` | 查询 `dtb` |
| 后续 store | 查询 `sttlb` | 查询 `dtb` | 查询 `dtb` | 查询 `stb` |

这个例子展示了两种方案的取舍：

- RTL 用更大的总 L1 容量换取更少的 load/store 互相驱逐；
- 修改前 gem5 用共享 entry 换取同页 load/store 的直接复用。

## 7. 总结

四种可比较的实现可以分别概括为：

```text
RTL:       load -> ldtlb
           store -> sttlb

修改前 gem5: load/store -> dtb

修改后 gem5 默认: load/store -> dtb

修改后 gem5 split: load -> dtb
                  store -> stb
```

修改后的 gem5 默认采用共享 `dtb`，所以未指定参数的既有命令仍保持原有容量、共享 entry 和同页复用行为。只有显式传入 `--enable-store-tlb`，才采用 load/store L1 分离。

RTL 和开启 split 模式的 gem5 都采用 load/store L1 分离，优点是有效 L1 容量更大、mixed workload 中跨类型污染更少；缺点是同一 VPN 的 load 和 store 不能直接共享 L1 entry，store 可能再次 lookup、miss 和 replay。

修改前 gem5 和修改后的默认模式更简单，优点是同页 load/store 可以直接复用一个 entry；缺点是所有 data translation 竞争一个 48-entry `dtb`，可能在分离工作集上产生更多跨类型淘汰和 miss。修改后的 split 模式修正了这个 L1 所有权问题，但仍是行为级对齐，不是 cycle-exact RTL 模型。

因此，比较这些实现时应先看访问模式：工作集按 load/store 分开且较大时，RTL 和开启 split 模式的 gem5 更有优势；load/store 高频访问相同页面时，修改前 gem5 和修改后的默认模式更容易复用共享 `dtb`，RTL 和开启 split 模式的 gem5 则可能承担额外的 store-side lookup/replay。
