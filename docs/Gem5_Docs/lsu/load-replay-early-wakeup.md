# C_MA 和 C_FF 的 load replay 提前唤醒

load 有时要等一条更老的 store。
- 预测到地址依赖时，load 要等 store 算出地址（`C_MA`）。
- 如果已经知道该从哪条 store 转发数据，但数据还没写入 Store Queue（SQ），load 就要等数据（`C_FF`）。

原先，load 要等地址或数据就绪后才能重试。现在，store 的地址微操作（STA）或数据微操作（STD）一旦赢得发射端口，就能给等待它的 load 发出提示。满足重试条件后，`retryMem()` 把 load 放入发射队列的 `replayQ`。这条 load 赢得发射端口后，会在下一拍重新进入 load 流水线的 S0 级。随后，访存队列（LSQ）会重新检查地址依赖和转发条件，如果仍需等 store，`InstructionQueue` 会按这次的原因重新登记这条 load。等地址的进 `mdpAddrReplayLdInsts`，等数据的进 `stlfFailLdInsts`。再次满足重试条件后，它才回到所属 `IssueQue` 的 `replayQ` 竞争发射端口。[重试入队](../../../src/cpu/o3/issue_queue.cc#L1007) · [发射仲裁](../../../src/cpu/o3/issue_queue.cc#L903)

## 1. store 发射后，load 哪一拍能收到提示

以 store 微操作赢得端口、由 `IssueQue::issueToFu()` 送往执行单元的那一拍为 `T`。STA 的提示对应香山 OG1 的地址唤醒，STD 的提示对应 OG0 的数据唤醒。[正常发射](../../../src/cpu/o3/issue_queue.cc#L982) · [重发路径](../../../src/cpu/o3/issue_queue.cc#L933)

| store 在 `T` 拍做什么 | load 哪一拍收到提示 | load 在等什么 |
| --- | --- | --- |
| STA 正常发射 | `T+1` | `C_MA`，等地址 |
| STD 正常发射 | KMHV3 默认是 `T` | `C_FF`，等数据 |
| STA 或 STD 从 `replayQ` 重新发射 | 两者都是 `T+1` | STA 唤醒等地址的 `C_MA`，STD 唤醒等数据的 `C_FF` |

`scheduleToExecDelay` 会影响 STA 和 STD 从调度到发射要走几拍。表里的 `T` 已经是赢得端口的那一拍。正常发射时，STA 的提示固定在 `T+1`。STD 的 OG0 提示要等到调度后的下一拍才可见。KMHV3 默认值为 2，STD 赢得端口时已经过了一拍，可以当拍给出提示。改成 1 后，STD 可以在调度当拍赢得端口，提示仍要等到 `T+1`。[发射流水](../../../src/cpu/o3/issue_queue.cc#L1363) · [STA 和 STD 的提示延迟](../../../src/cpu/o3/issue_queue.cc#L983)

## 2. C_MA 等哪条 store 的地址

启用 `EnableReplayBasedMDP` 后，LSQ 会在尝试转发或访问 cache 前检查内存依赖预测器（MDP）的结果。如果 load 需要等地址，发射队列（IQ）就记下它在等哪条 store 的提示。即使预测器给出了多条依赖，每条等待提前唤醒的 load 也只匹配一条 STA。[LSQ 的依赖检查](../../../src/cpu/o3/lsq_unit.cc#L3667) · [IQ 登记等待项](../../../src/cpu/o3/inst_queue.cc#L1182)

| 等待模式 | 哪条 store 的 STA 可以提前唤醒 load | 如果没有提前提示，load 要等到什么时候 |
| --- | --- | --- |
| 普通等待 | 从预测器给出的 store 中，选地址还没就绪、序列号最大的一条 | 等待集合里所有 store 的地址依赖都已解除 |
| 严格等待 | load 入队时，排在它前面的最后一条 store | `storeCompletedIdx` 达到或超过这条 store 的 SQ 索引 |

严格等待用 SQ 索引跟踪完成进度，用 store 序列号匹配提示。load 入队时把 `storeQueue.end()` 存入 `sqIt`，它指向当时队尾的后一位。因此，`sqIt.idx() - 1` 就是这条 load 之前最后一条 store 的索引，也是 `storeCompletedIdx` 需要达到的值。如果检查时 SQ 队头已经越过这条 store，load 就继续执行，无需再从已出队的表项里取序列号。[保存 SQ 队尾](../../../src/cpu/o3/lsq_unit.cc#L811) · [严格等待登记](../../../src/cpu/o3/lsq_unit.cc#L3670)

地址提示可能在 load 退出流水线前就到达。收到提示时，IQ 先检查这条 store 是否在物理 SQ 的可写窗口内，再按线程和 store 序列号找到等待的 load，将 `earlyReady` 置位。等 load 走到 `loadWhenToReplay`，IQ 会将 `pipeDone` 置位。两个标志都已置位，IQ 才调用 `retryMem()` 让 load 提前重试。[物理 SQ 可写窗口](../../../src/cpu/o3/lsq_unit.cc#L1441) · [处理地址提示](../../../src/cpu/o3/inst_queue.cc#L1137) · [load 走到重试位置](../../../src/cpu/o3/lsq_unit.cc#L1927)

## 3. C_FF 何时能在收到提示的同一拍重试

转发失败时，LSQ 已经找到了应该提供数据的 store。IQ 记下这条 store 的序列号，也记下 load 进入等待队列的时刻 `enqueueTick`。只有同一线程里这条 store 的 STD 提示，才会让 IQ 将该 load 的 `earlyReady` 置位。[转发失败入队](../../../src/cpu/o3/inst_queue.cc#L996) · [匹配 STD](../../../src/cpu/o3/inst_queue.cc#L1125)

假设 STD 提示在第 10 拍到达。load 能否赶上这一拍的端口仲裁，取决于它在本拍开始时是否已经入队。

| load 何时进入 C_FF 等待队列 | 第 10 拍收到提示时 | 最早参加重试仲裁的时刻 |
| --- | --- | --- |
| 第 9 拍或更早 | load 已经在等，IQ 可以当拍调用 `retryMem()` | 第 10 拍 |
| 第 10 拍 | IQ 记下提示，到下一拍才满足 `enqueueTick < curTick()` | 第 11 拍 |

KMHV3 的队列列表中，`ld0` 排在 `std0` 和 `std1` 前面。按列表顺序仲裁，STD 发出提示时，load 已经错过了本拍的端口。

所以，只要本拍开始时还有 C_FF load 在等，调度器走到第一个 load 端口时就先处理后面的 STD 队列。随后调用 `retrySTLFFailLoads(true)`，其中 `true` 表示只把本拍之前已入队、又已收到提示的 load 送去重试。这些 load 随即可以参加本拍的端口仲裁。调度器继续往后走时会跳过已经处理过的 STD 队列。[KMHV3 队列顺序](../../../configs/common/FUScheduler.py#L220) · [调度顺序](../../../src/cpu/o3/issue_queue.cc#L2035) · [重试条件](../../../src/cpu/o3/inst_queue.cc#L1080)

延迟送达的 STD 提示在到达的那一拍始终有效，同拍新入队的 load 也能收到。提示过期或对应的 store 被 squash 后，IQ 会将它清除。[提示匹配与清理](../../../src/cpu/o3/inst_queue.cc#L1137)

IQ 常规扫描时也会检查 store 数据是否已经就绪。若物理 SQ 暂时不可写，STD 即使已经发出提示，也得推迟写入。只有对应 SQ 项的 `dataReady()` 为真，IEW 才把等待项标为 `resolved`。在此之前，收到提示的 load 可以先重试一次。[STD 执行完成](../../../src/cpu/o3/iew.cc#L2542)

## 4. 在 trace 里看 load 哪一拍真正发射

下面三对指令取自开发期间保存的 `--ideal-kmhv3` SE trace。这次测试使用普通等待模式，每个 CPU 周期是 333 个 gem5 tick。按序列号跟踪同一条 load，`retry MemRead` 是它进入重试队列的时刻，`replayed to FU` 是它赢得端口、发往执行单元的时刻。

| 路径 | store / load 序列号 | load 登记等待 | STA / STD 发射 | load 调用 `retryMem()` | load 发射 |
| --- | --- | ---: | ---: | ---: | ---: |
| C_MA，`pipeDone` 已置位 | 1218 / 1222 | 161172 | 162171 | 162504 | 162504 |
| C_FF，提示前已入队 | 3527 / 3528 | 264069 | 265734 | 265734 | 265734 |
| C_FF，提示当拍入队 | 4627 / 4628 | 313020 | 313020 | 313353 | 313353 |

第一条 load 在 tick 161505 就已走到重试位置，`pipeDone` 已置位，等 STA 发射一拍后就能重试。后两条 load 都在等数据。第二条收到提示时已在队列中，当拍就能重试。第三条当拍才入队，要等下一拍。

这次 trace 里，和第二条情况相同的 27 个样本都在提示当拍进入了重试队列，只有 24 个当拍拿到了端口。

本地 RISC-V 测试可以用两段指令分别触发这两种等待。一段延迟 store 的地址计算，另一段延迟要写入的数据。两段都在 store 后面放一条读同一地址的 load。把 `REPLAY_ELF` 设为测试程序的 ELF 路径，在仓库根目录运行当前分支编译的 gem5。

```bash
build/RISCV/gem5.opt -d m5out/replay-wakeup \
    --debug-flags=Commit,IQ,IEW,LoadPipeline,Schedule,StorePipeline \
    --debug-file=replay.gz configs/example/se.py --ideal-kmhv3 -c "$REPLAY_ELF"
```

读 trace 时，先找 load 开始等待的那一行。等地址时看 `MDP addr replay register`，等数据时看 `unable to forward from data unready store`。记下这一对 store 和 load 的序列号，把 store 的 `add to FUs` 与 load 的 `retry MemRead`、`replayed to FU` 按 tick 对起来。

开发时用 StoreSet 验证严格等待，参数为 `EnablePHASTMDP=False`、`EnableMDPStrictWait=True`、`LFSTEntrySize=1`。trace 中出现 `MDP strict addr replay register` 时，这条 load 已进入严格等待。要看两种原因各触发了多少次 replay，可以查 `stats.txt` 中的 `loadReplayEvents::MdpAddrReplay` 和 `loadReplayEvents::STLFReplay`。要判断某次重试是否提前，还要对照 trace 里的发射和重试时刻。
