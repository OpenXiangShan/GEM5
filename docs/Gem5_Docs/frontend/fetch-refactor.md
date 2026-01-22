# fetch.cc 简化/重构第一阶段计划（强制 Decoupled + BTB，保留 ICache 逻辑）

## 背景与目标
当前 `src/cpu/o3/fetch.cc` 同时支持：
- decoupled 前端（BTB/FTB/Stream predictor）
- coupled 前端（传统 `branchPred->predict/update/squash`）

本阶段目标是把 fetch 侧收敛到**唯一模式**：`decoupled frontend + DecoupledBPUWithBTB`，并尽量做到**行为/性能基本不变**。

## 本阶段明确目标（Goals）
- 运行时强制：只能使用 `decoupled + BTB`（不再支持 coupled / Stream / FTB）。
- 删除 fetch 侧所有 coupled/Stream/FTB 分支代码，使 fetch.cc/fetch.hh 更短、更单一。
- 保留现有 ICache/TLB/双 cacheline 请求逻辑与时序（不碰 fetchBuffer/multi-cacheline 合并等）。
- 保留现有 `decoupledPredict()` 驱动 FTQ 消费的机制（不改 FTQ/FSQ 交互边界）。

## 本阶段不做（Non-goals）
- 不简化 ICache 交互：`handleMultiCacheLineFetch()` / `processMultiCacheLineCompletion()` 保持原样。
- 不修改 `fetchBufferSize=66`、不改取指跨 cacheline 的策略。
- 不重构状态机（`ThreadStatus/CacheRequestStatus` 保持）。
- 不合并 FTQ/FSQ，不重写 `decoupledPredict()` 逻辑与 predictor 内部实现。
- 不做 “按 FetchBlock 一拍必完成/下一拍必进入下一项” 的时序承诺（ICache miss/ITLB miss 仍会影响节拍推进）。

## 关键设计约束（Assumptions）
- 后续不会再使用 coupled BPU，也不会再使用 Stream/FTB。
- FetchBlock 固定为 64B（最多 32 条指令），BTB 预测已按此策略产出 FTQ entry（`FtqEntry{startPC,endPC,takenPC,taken,target,...}`）。
- 性能“基本不变”的含义：相同配置下 IPC 与关键统计无明显退化；任何退化应可解释且可回滚。

---

## 执行计划

### Phase 0：把“只能 BTB+decoupled”变成显式前提（Fail-fast）
**目的**：先确保任何非目标配置都在启动时明确失败，而不是运行到半路触发空指针/隐式 fallback。

**修改点**
- `src/cpu/o3/fetch.cc`：`Fetch::Fetch(...)` 构造阶段完成 `branchPred` 绑定后：
  - `assert(branchPred);`
  - `assert(branchPred->isDecoupled());`
  - `assert(branchPred->isBTB());`
  - `dbpbtb = dynamic_cast<branch_prediction::btb_pred::DecoupledBPUWithBTB*>(branchPred); assert(dbpbtb);`
  - `dbpbtb->setCpu(cpu);`
- 对 trace mode：
  - 若 trace mode 禁止 decoupled frontend（`!traceFetch->allowDecoupledFrontend()`），直接 `fatal()`（避免静默回退到 coupled 语义）。

**验收**
- 非 decoupled 或非 BTB 配置在启动时明确报错退出。
- 目标配置可继续运行。

---

### Phase 1：删除 coupled 前端分支（fetch 侧）
**目的**：删除 `!isDecoupledFrontend()` 的旧路径，使 fetch 只存在“按 FTQ entry 取指”的逻辑。

**典型删除范围（示例位置）**
- `src/cpu/o3/fetch.cc`: coupled 的 `branchPred->predict(...)` 分支
- `src/cpu/o3/fetch.cc`: coupled 的 commit update/squash 分支（`branchPred->update/squash`）
- `src/cpu/o3/fetch.cc`: coupled 的 decode squash 分支（`branchPred->squash`）
- `src/cpu/o3/fetch.cc`: `sendNextCacheRequest()` 中 non-decoupled else 分支（仅保留 decoupled/FTQ entry 取指请求路径）

**验收**
- 编译通过（`build/RISCV/gem5.opt`）。
- 最小 workload 可跑通并输出 stats。

---

### Phase 2：移除 Stream/FTB 在 fetch 侧的所有分支与成员
**目的**：fetch 只维护 BTB predictor 指针 `dbpbtb`，彻底移除 `dbsp/dbpftb` 以及 `isStreamPred/isFTBPred` 分支。

**修改点**
- `src/cpu/o3/fetch.hh`
  - 删除 `dbsp`、`dbpftb` 成员与相关 include（`cpu/pred/ftb/...`、`cpu/pred/stream/...`）。
  - 删除/收敛 helper：移除 `isStreamPred()` / `isFTBPred()`，只保留 BTB（或直接不再需要类型判断）。
  - 清理任何 FTB-only 类型成员（例如带 `ftb_pred::...` 的字段）；若该字段只用于旧实验/复制粘贴残留，应直接删除或先注释为后续清理点。
- `src/cpu/o3/fetch.cc`
  - `resetStage()`：只保留 `dbpbtb->resetPC(...)`
  - `checkDecoupledFrontend()`：只保留 `dbpbtb->fetchTargetAvailable()` 分支
  - `lookupAndUpdateNextPC()`：只保留 `dbpbtb->decoupledPredict(...)`
  - `updateBranchPredictors()`：只保留 `dbpbtb->tick()` + `trySupplyFetchWithTarget(...)`
  - `buildInst()`：只从 `dbpbtb` 读取 `getSupplyingStreamId()/getSupplyingTargetId()`
  - `getNextFTQStartPC()`：只从 `dbpbtb->getSupplyingFetchTarget()` 读取 entry startPC/endPC 信息
  - commit/decode squash/update：只保留 `dbpbtb->{controlSquash,nonControlSquash,trapSquash,update}`

**验收**
- `src/cpu/o3/fetch.*` 中 `dbsp/dbpftb/isStreamPred/isFTBPred` 相关符号为 0 命中。
- 编译 + 最小 workload 运行通过。
- 关键统计/IPC 与 Phase 1 前后对比无明显退化。

---

### Phase 3：只加小规模一致性断言（不改机制）
**目的**：提高“删代码后仍保持语义”的可信度，并在未来重构前把不变量固定住。

**建议断言点（示例）**
- fetch 运行关键路径中：用 `assert(dbpbtb)` 替代宽泛的 `assert(isDecoupledFrontend())`（因为现在就是强制 BTB）。
- 将原本语义过载的 `usedUpFetchTargets` 拆成两个更明确的状态位：
  - `needFtqSupply`：需要 predictor supply 新 FTQ entry
  - `exhaustedFtqEntry`：当前 FTQ entry 已消费完，需要推进到下一个 entry
  并用 `ftqEmpty()` 统一表示“当前无法继续按 FTQ 推进取指”。

**验收**
- Debug flags（`Fetch/DecoupleBP`）下观察 FTQ entry 推进与 cache request 发起仍正常。
- 不引入新的死循环/长期 stall（例如 FTQ 不前进）。

---

## 验证/回归方法（每个 Phase 后都做）
1. 编译：
   - `scons build/RISCV/gem5.opt -j<N>`
2. 功能：
   - 跑你现有的最小 dummy / checkpoint 脚本（保持与 baseline 相同参数）
3. 性能/统计对比（同输入、同配置）：
   - IPC：`cpu.ipc`
   - Fetch 统计：`fetch.*`
   - BTB predictor 统计：`dbpbtb*`（或相关 group）
4. 必要时打开 debug：
   - `--debug-flags=Fetch,DecoupleBP`（只在定位问题时使用）

---

## 风险与回滚策略
- **风险**：删 FTB/Stream 可能牵连到 fetch 中遗留的 FTB-only 字段或训练路径，导致编译失败或行为变化。
- **策略**：
  - Phase 0 先 fail-fast，保证不支持的配置不会“跑着跑着坏掉”。
  - 每个 Phase 后都能编译/跑通；若出现性能回归，优先定位是否误删了 BTB 路径中的必要逻辑。
  - 建议在开始 Phase 1 前打一个 git tag/分支保存当前状态（尤其如果 FTB upperBound 需要长期可复现）。

---

## 预计收益
- fetch 侧分支大幅减少（只剩 decoupled+BTB 一条路径），后续做“按 FetchBlock/FTQ entry 驱动的进一步简化”和“FSQ/FTQ 合并”时复杂度显著下降。
- 不再需要维护基本不用的 FTB/Stream/coupled 兼容逻辑，长期可维护性更好。

---

## 实施记录（已完成）
- Phase 0/1/2 已执行并编译通过：`scons build/RISCV/gem5.opt`
- 关键改动文件：
  - `src/cpu/o3/fetch.hh`: 移除 Stream/FTB 成员与 include，仅保留 `dbpbtb`
  - `src/cpu/o3/fetch.cc`: 强制 decoupled+BTB、删除 coupled/Stream/FTB 分支，`sendNextCacheRequest()` 仅按 FTQ entry 发 icache 请求
  - `src/cpu/o3/trace/TraceFetch.cc`: 同步移除对 `dbsp/dbpftb` 的引用，按 BTB prime/读取 FTQ
- 进一步删减：
  - 已删除 `Fetch::isDecoupledFrontend()` 及所有调用点
  - `usedUpFetchTargets` 已拆分为 `needFtqSupply` + `exhaustedFtqEntry`，并新增 `ftqEmpty()`
  - 删除了一批高频/低价值的 `DPRINTF(Fetch, ...)`，保留关键事件日志，并将部分下调到 `FetchVerbose`

---

## Phase 4（讨论/待确认）：简化 Fetch<->BPU 交互与 FTQ 数据结构（BTB-only）

### 背景：BTB predictor 已趋于 “1 FSQ entry <-> 1 FTQ entry”
历史上 stream predictor 会把一个 FSQ/stream 切成多个 FTQ entry，因此 FTQ 内部通常需要较复杂的：
- enqueue 状态（例如 `FetchTargetEnqState{pc,streamId,nextEnqTargetId}`）
- supply 状态（例如 `supplyFetchTargetState{valid,targetId,entry*}` + `fetchDemandTargetId`）
- 以及 `trySupplyFetchWithTarget(demand_pc)` 这种“根据 demand 对齐 head，并可能跳过 entry”的逻辑

但在当前 BTB predictor 路径中，预测器基本保证 **一个 FSQ entry 对应一个 FTQ entry**（或至少 fetch 侧可以按这个不变量来设计/断言）。
这使得 “FTQ 作为一个严格 FIFO 队列” 的实现与接口更合理，也更接近 RTL 实现形态（队列 + squash flush）。

### 目标（对 fetch 视角的最小契约）
让 fetch 只依赖最小交互接口，BPU 只负责“产出 entry 入队”，fetch 纯消费：
- `tick()`：BPU 每拍推进自身流水并入队新 entry（若有）
- `bool hasHead()`：是否有可消费的 head entry
- `const FtqEntry& head()` / `peekHead(out)`：读取 head entry（只读，无副作用）
- `void popHead()`：当 fetch 消费完当前 entry 后推进队列
- `void squash(...)`：遇到 redirect/squash 时 flush 队列并重置到新起点
- （可选）`void onEntryConsumed(fsqId/ftqId, fetchedInstNum, ...)`：把“本 entry 实际 fetch 了多少 inst”等信息反馈给 BPU（替代现有 `decoupledPredict()` 里的部分统计/写回）

> 这套契约下，fetch 不再需要“supply/re-supply”概念；fetch 只是在 `hasHead()` 为真时消费队列头。

### 关键不变量（你已倾向硬 assert）
为了删掉 “demand_pc >= endPC 触发跳过 entry” 这类自愈分支，约束不变量为：
- 当 `hasHead()` 为真时，fetch 的当前 `pc` 必须满足：`head.startPC <= pc < head.endPC`
- 若 `pc >= head.endPC`，说明 entry 已耗尽：必须先 `popHead()` 再继续
- 任何违反上述条件的情况视为 bug（`assert` / `panic`）

这能显著简化 FTQ 内部状态机与分支数量。

### 与 “把 BPU 更新放到 fetch 之前” 的关系
如果 BPU 在每拍开始 `tick()` 并尽量提前产出/入队 head：
- 绝大多数周期 fetch 进入本拍时就能直接 `hasHead()->head()`，无需额外对齐/供给逻辑
- entry 边界处的“re-supply”自然退化为：`popHead()` 后继续读取下一个 `head()`（仍可实现同拍发出下一次 ICache 请求）
- fetch 的控制流更像 RTL：队列头驱动 ICache 请求与 PC 范围校验

### 对现有模块的简化方向（不改功能的重构目标）
1. `FetchTargetQueue`：从 “map + supply/enq state” 简化为 “queue（deque/ring）”
   - `fetchTargetAvailable()` => `!queue.empty()`
   - `getTarget()` => `queue.front()`
   - `finishCurrentFetchTarget()` => `queue.pop_front()`
   - 删除 `supplyFetchTargetState` 与 `fetchDemandTargetId`（以及相关对齐逻辑）
2. `FetchTargetEnqState`：
   - BTB-only + 1:1 情况下，enqueue 不再需要“在一个 stream 内移动 pc/streamId”；可以逐步删除或仅保留调试字段
3. 删除/替换 `trySupplyFetchWithTarget(demand_pc)`：
   - 改为无副作用的 `hasHead/peekHead`
   - 任何 “demand_pc 已越界所以跳 entry” 行为改为硬断言（你已倾向）

### 为后续删除 `decoupledPredict()` 的铺垫（Phase 5 方向）
当前 `decoupledPredict()` 混合了：
- fetch 侧 nextPC 更新（控制流）
- BPU 侧 entry 生命周期推进（finish/pop）与统计（fetchedInstNum 等）

若要删除它，建议拆成：
- fetch：完全用 `FtqEntry{startPC,endPC,takenPC,taken,target}` 计算 nextPC，并判断 entry exhausted
- BPU：只保留 “产出 entry 入队” + “接收 fetch 消费结果（pop + fetchedInstNum）” 的接口


## 实施记录（已完成）
- Phase 4/5 已执行并编译通过, IPC 没有太大变化