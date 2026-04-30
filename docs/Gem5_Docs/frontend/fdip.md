# FDIP：FTQ-directed ICache 预取

## 背景

FDIP（Fetch Directed Instruction Prefetch）在当前 XS-GEM5 分支中用于缓解
decoupled frontend 的 ICache miss 停顿。现有前端由 BPU/FTQ 产生取指目标，
Fetch 再根据 FTQ entry 访问 ICache。FDIP 复用这条预测目标流，在 demand fetch
真正需要某些 cache line 之前，尽力把它们送入 ICache 路径。

当前实现边界必须明确：这是 Phase 1 / 1.5 的 FTQ-directed ICache prefetch，
不是完整 RTL 对齐实现，也不是 paper-faithful 的 FDIP 复刻。FDIP 只影响
best-effort 性能路径；`fetchptr` 驱动的 demand fetch 仍然是 correctness path。
当资源不足、地址不可预取、redirect 发生或 epoch 失配时，FDIP 可以丢弃或忽略
prefetch，不得阻塞 demand fetch，也不得改变程序正确性。

## 核心代码位置

当前 FDIP 实现横跨 Fetch、BPU/FTQ、Request metadata、BaseCache 和 Python 配置：

| 位置 | 作用 |
| --- | --- |
| `src/cpu/o3/fetch.cc` / `src/cpu/o3/fetch.hh` | Fetch 侧 FDIP 调度、line coverage 计算、translation、pending/outstanding 状态、probe hint 消费和统计 |
| `src/cpu/pred/btb/ftq.cc` / `src/cpu/pred/btb/ftq.hh` | FTQ entry 与 `fetchptr` / `prefetchptr` 相关控制状态 |
| `src/cpu/pred/btb/decoupled_bpred.cc` / `src/cpu/pred/btb/decoupled_bpred.hh` | `DecoupledBPUWithBTB` 的 FDIP 开关、lookahead/issue/outstanding 参数和 FTQ helper 接口 |
| `src/mem/request.hh` | `Request::XsMetadata` 中携带 FDIP epoch、FTQ id、start PC、selected-way hint |
| `src/mem/cache/base.cc` / `src/mem/cache/base.hh` | L1I FDIP request 识别、direct probe、MSHR/安装策略、old-path refill drop、useful/late/unused 统计 |
| `configs/common/Options.py` | 命令行参数定义 |
| `configs/common/xiangshan.py` | 把命令行参数连接到 CPU/BPU/Cache SimObject |
| `configs/example/kmhv3.py` | 当前 tuned-on 研究配置入口，启用 FDIP 相关 ICache MSHR 分配和 accessor |

## 关键数据结构

### FTQ 与预测目标

FTQ 中的 entry 描述一个预测取指窗口，核心字段包括 `startPC`、`endPC`、
`takenPC`、`target` 和对应的 stream/target id。demand fetch 使用 `fetchptr`
消费当前 entry；FDIP 使用独立的 `prefetchptr` 查看更前方的 entry。redirect
或 squash 后，`prefetchptr` 必须和新的 demand 路径重新对齐，不能继续沿旧路径
预取。

### Fetch 侧 FDIP 状态

Fetch 维护每线程 FDIP 状态、pending request、outstanding line 计数、
candidate/issued line 去重信息和 probe hint。`Fetch::runFdip(tid)` 是核心调度点，
它只在 FDIP 开启且资源允许时查看一个 future FTQ target，并按照实际 fetch coverage
生成 cacheline 粒度的预取请求。

### Request metadata

FDIP request 通过 `Request::XsMetadata` 把 fetch 侧上下文传递到 cache：

- `fdipEpoch`：标识 request 所属前端生命周期，用于 redirect 后识别 old-path refill。
- `fdipFtqId`：标识来源 FTQ entry，便于统计和调试。
- `fdipStartPC`：记录 FDIP target 起始 PC。
- `fdipSelectedWayValid`、`fdipSelectedWay`、`fdipSelectedWayTick`：direct probe 命中后传递 selected-way hint。

### BaseCache 侧 FDIP 记录

BaseCache 只把 FDIP 策略作用在 FDIP-scoped request 上。它维护 FDIP line 的安装、
useful/late/unused 分类、recent-unused suppression、direct probe 和 miss allocation
分类。demand fetch 不受 recent-unused suppression 影响。

## 数据流

![](../images/fdip-data-flow.svg)

图中蓝色主线是 FDIP prefetch path，绿色主线是 demand correctness path，紫色虚线是
direct probe / selected-way hint，橙色虚线是 redirect / epoch cleanup，红色虚线是
old-path refill drop。

整体流程如下：

1. BPU 生成预测结果并写入 FTQ；`fetchptr` 供 demand fetch 使用，`prefetchptr`
   供 FDIP runahead 使用。
2. `Fetch::runFdip(tid)` 查看 `prefetchptr` 指向的 future target，并受
   `fdip_issue_bandwidth`、`fdip_max_outstanding` 和 lookahead 限制。
3. `computeFdipLineAddrs(...)` 按 demand fetch 相同的 actual fetch coverage 计算
   cacheline。当前默认不是固定 66B overfetch，而是覆盖真实 `[startPC, predEndPC)`
   以及必要的 control-tail 范围。
4. Fetch 为每条 FDIP line 建立带 `XsMetadata` 的 inst-fetch prefetch request，
   经过地址翻译后送到 L1I。
5. L1I 可以通过 direct probe 判断 line 是否已经存在；probe hit 可以完成 FDIP work，
   或产生 selected-way hint，而不必分配真实 miss。
6. 如果 FDIP miss 需要下级访问，BaseCache 使用 FDIP-scoped MSHR/安装与分类统计。
7. demand fetch 后续访问同一 line 时，cache 侧把它分类为 useful、late 或 unused
   等结果，用于评估 FDIP 是否真的提前隐藏了 ICache miss。

## Redirect cleanup / old-path refill drop

FDIP 的 redirect 语义依赖 epoch。Fetch 在 redirect、squash 或 reset 时会更新
FDIP epoch，并清理当前线程的 partial state、pending request、outstanding accounting
和 probe hint。旧 epoch 的 translation 或 response 回来时，fetch 侧必须忽略它们。

当启用 `--fdip-drop-refill-on-epoch-mismatch` 时，BaseCache 在安装 refill 前检查
FDIP metadata。如果 refill 属于旧 epoch，且该请求是 FDIP traffic，则不把它安装进
L1I。这个策略只对 FDIP 生效，不能影响 demand fetch refill。

recent-unused suppression 也是 FDIP-only 策略。它按物理 cacheline 和 security domain
识别最近一次生命周期为 unused 的 line，在冷却窗口内抑制 FDIP 再次发起同一 line；
demand fetch 永远不能被该机制过滤。

## 配置参数

常用命令行参数如下：

| 参数 | 语义 |
| --- | --- |
| `--enable-fdip` | 开启 FTQ-directed ICache prefetch，默认关闭 |
| `--bpu-runahead-entries=N` | 限制 BPU runahead 与 demand fetch 的 FTQ 距离；`0` 表示不限制 |
| `--fdip-lookahead-entries=N` | 限制 FDIP `prefetchptr - fetchptr` 的距离 |
| `--fdip-issue-bandwidth=N` | 每周期最多发出多少条 FDIP cacheline request |
| `--fdip-max-outstanding=N` | FDIP 最多允许多少条 outstanding cacheline request |
| `--prefetch-lines-per-ftq=start_line_only\|cover_actual_fetch_range` | 选择只预取起始 line，或覆盖实际 fetch coverage |
| `--fdip-flush-partial-on-epoch-change` | epoch 变化时清理 partial FDIP state，当前推荐保持开启 |
| `--no-fdip-flush-partial-on-epoch-change` | 保留 partial state 的实验开关，当前 FDIP-on 路径不推荐使用 |
| `--fdip-drop-refill-on-epoch-mismatch` | redirect 后丢弃 old-path FDIP refill 安装 |
| `--fdip-recent-unused-cycles=N` | 对最近 unused 的 FDIP line 做冷却抑制；`0` 表示关闭 |

当前研究配置通常从 `configs/example/kmhv3.py` 进入。启用 FDIP 时，该配置会调整
ICache MSHR 分配，并把 `cpu.fdipIcacheAccessor` 连接到 `cpu.icache`，用于 direct probe
和 recent-unused 检查。

## 统计观察

Fetch 侧主要观察 FDIP 是否发出、是否被过滤、是否重复、是否走错路径：

- `system.cpu.fetch.fdipIssuedLines`
- `system.cpu.fetch.fdipDropped`
- `system.cpu.fetch.fdipFilteredFault`
- `system.cpu.fetch.fdipFilteredUncacheable`
- `system.cpu.fetch.fdipFilteredRecentUnused`
- `system.cpu.fetch.fdipCandidateLines`
- `system.cpu.fetch.fdipDirectProbeHit`
- `system.cpu.fetch.fdipWrongPathIssuedLines`
- `system.cpu.fetch.fdipEpochMismatch`
- `system.cpu.fetch.icacheStallCycles`

L1I 侧主要观察 FDIP 是否产生真实收益或污染：

- `system.cpu.icache.fdipInstalled`
- `system.cpu.icache.fdipUsefulHits`
- `system.cpu.icache.fdipLate`
- `system.cpu.icache.fdipUnused`
- `system.cpu.icache.fdipEpochMismatch`
- `system.cpu.icache.fdipDroppedRefill`
- `system.cpu.icache.fdipProbeHit`
- `system.cpu.icache.fdipProbeMerged`
- `system.cpu.icache.fdipMissAllocAfterUnused`
- `system.cpu.icache.fdipMissAllocAfterUnusedThenUseful`
- `system.cpu.icache.fdipMissAllocAfterUnusedThenUnused`

评估 FDIP 时，不应只看 IPC。建议同时看 ICache miss latency、`icacheStallCycles`、
`fdipUsefulHits`、`fdipLate`、`fdipUnused` 和 repeated bad line 相关计数。当前 P0
调优的主要收益来自 recent-unused suppression 和 old-path refill drop 对重复无用 line
的压制。

## 限制与验证

当前模型仍有以下限制：

- 不是完整 RTL tag/data split ICache 接口。
- `prefetchptr` / FTQ peek 只覆盖当前研究所需的 Phase 1 / 1.5 行为。
- redirect cleanup 主要有 helper-level witness，不等价于完整 mid-flight redirect harness。
- `system.cpu.iew.fetchStallReason::IcacheStall` 在部分 trace-mode 运行中仍可能保持为零；
  目前更可信的 fetch 侧停顿观测点是 `system.cpu.fetch.icacheStallCycles`。
- `kmhv3.py` 是当前最完整的 tuned-on 配置路径，其他入口还没有同等文档化。

建议的最小验证组合：

```bash
openspec validate add-fdip-icache-prefetch --strict
scons build/RISCV/gem5.opt -j8
scons build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt --unit-test -j8
build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt
scons build/RISCV/cpu/o3/fdip_cleanup.test.opt --unit-test -j8
build/RISCV/cpu/o3/fdip_cleanup.test.opt
```

研究验证还应至少包含一个 FDIP-off 对照、一个 FDIP-on smoke，以及高 ICache 压力 trace
上的 useful/late/unused 和 miss-latency 观察。FDIP 默认关闭时，应保持功能、性能路径和
统计行为与基线一致。
