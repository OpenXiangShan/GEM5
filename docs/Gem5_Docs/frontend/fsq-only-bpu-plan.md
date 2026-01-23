# FSQ-only 的 BTB BPU 重构计划（删除 FTQ，对齐 RTL）

## 背景
当前 BTB-only 的 decoupled frontend 在实现上基本已经形成严格的 1:1 关系：
- 一个 FSQ entry（`FetchStream`）对应一个 FTQ entry（`FtqEntry`）。
- `ftqId = fsqId - 1`（历史遗留的 ID 偏移）。

目前 gem5 同时保留了两套结构：
- **FSQ**：`fetchStreamQueue` 保存完整元信息（用于 resolve/commit/update 等训练与统计）。
- **FTQ**：`FetchTargetQueue` 保存一份“最小投影”，fetch 用它来驱动 `PC` 范围检查与 taken/target 决策。

在 RTL 中（至少在 BTB-only + 1:1 的前提下），这类职责通常可以由**一个 queue**承担。我们的目标是简化并彻底删除 FTQ，让 **fetch 直接消费 FSQ（FetchStream）**。

本文档只记录任务规划与分阶段推进方式（本阶段不改代码）。

## 目标（Goals）
- 删除 BTB 预测器中的 **全部 FTQ 内容**：移除 `FetchTargetQueue` 类以及 `DecoupledBPUWithBTB` 对它的依赖。
- 让 **fetch 直接消费 `FetchStream`**（FSQ head-driven）。
- 暂时保持外部接口里的 legacy ID 习惯：
  - `ftqId = fsqId - 1`（接口仍可能传 `ftqId`，内部先兼容）。
- 不再使用 `ftq_size` 的容量/背压语义，只保留 `fsq_size`（单队列模型）。
- 减少代码量与状态机分支，降低复杂度，便于后续继续贴近 RTL。

## 非目标（Non-goals，本系列不做）
- 不重写/重设计各个 predictor 子模块（UBTB/TAGE/ITTAGE/MGSC/RAS 等）。
- 第一步不做 `FetchStream` 字段的语义大清理（结构稳定后再做）。
- 不承诺 IPC 完全不变：删除 FTQ 可能会减少内部“可见性”延迟（少一拍）。如需补回，会在其他位置（例如 fetch->decode 延迟）单独处理。

## 需要强制的关键不变量（assert/fatal）
- **FSQ ID 单调递增**：
  - 新 stream 入队时的 id 必须满足：`nextId = baseId + deque.size()`。
- **fetch 按顺序消费 FSQ head**：
  - 当 `fsqHeadId` 有效时，fetch 的 `pc` 必须在 `[stream.startPC, stream.predEndPC)` 之内。
- **legacy ID 映射**：
  - 当 `ftqId` 与 `fsqId` 同时出现时，强制 `ftqId == fsqId - 1`。

## 分阶段推进方案（推荐顺序）

### Phase 1：先把 FSQ 容器改成 `std::deque + baseId`（暂不删 FTQ）
**为什么先做**：这是机械重构，语义风险低；并且能把“队列 head”的概念显式化、O(1) 化，后续删 FTQ 会更简单。

**实现草案**
- 在 `src/cpu/pred/btb/decoupled_bpred.hh`：
  - 将：
    - `std::map<FetchStreamId, FetchStream> fetchStreamQueue;`
  - 改为类似：
    - `std::deque<FetchStream> fetchStreamQ;`
    - `FetchStreamId fetchStreamBaseId;`（对应 `fetchStreamQ.front()` 的 id）
    - `FetchStreamId fsqNextId;`（下一条入队要分配的 id，用于替代当前 `fsqId` 的用法）
- 在 BPU 内部提供私有 helper（建议集中封装，减少散落的 id/base 计算）：
  - `bool hasStream(FetchStreamId id) const;`
  - `FetchStream& getStream(FetchStreamId id);`
  - `const FetchStream& getStream(FetchStreamId id) const;`
  - `FetchStreamId frontId() const;`
  - `FetchStreamId backId() const;`
  - `void eraseAfter(FetchStreamId id);`（替代 `squashStreamAfter` 的 map/upper_bound 写法）
  - `void commitThrough(FetchStreamId id);`（替代 `update()` 里逐个 erase 的逻辑，改为 pop_front）
- 在 `src/cpu/pred/btb/decoupled_bpred.cc` 更新所有相关访问点：
  - `processNewPrediction()` 的入队路径。
  - `squashStreamAfter()`。
  - `update(stream_id, tid)` 的 commit 路径。
  - 所有 `find()/upper_bound()` 改为 deque 索引。
  - 需要访问 `fsqId - 1` 的地方（例如 abtb update）改成显式“取前一个 entry”（存在则用，不存在走 fallback）。

**验收**
- 编译通过，并能跑通（此阶段 FTQ 仍存在，所以外部行为应保持一致）。
- BTB predictor 内部不再出现 `std::map` 的 FSQ 存储。

---

### Phase 2：让 fetch 直接消费 FSQ head（接口迁移）
**为什么**：你现在的 `fetch.cc` 已经是 head-driven，不再依赖 `decoupledPredict()`；把“head”从 `FtqEntry` 切到 `FetchStream` 是直观的下一步。

**实现草案**
- 在 BPU 侧提供对 fetch 的接口（命名待定；为了减小 diff，可先保留旧名字并在内部转发）：
  - `bool fsqHasHead()`
  - `const FetchStream& fsqHead()`
  - `FetchStreamId fsqHeadId()`
  - `FetchTargetId headFtqId() { return fsqHeadId() - 1; }`
- 在 `src/cpu/o3/fetch.cc`：
  - `lookupAndUpdateNextPC()`：
    - 用 `FetchStream` 替代 `FtqEntry`：
      - start：`stream.startPC`
      - end：`stream.predEndPC`
      - taken：`stream.predTaken && (curr_pc == stream.predBranchInfo.pc)`
      - target：`stream.predBranchInfo.target`
  - `buildInst()`：
    - `setFsqId(fsqHeadId)`
    - `setFtqId(fsqHeadId - 1)`（保持 legacy offset）
-（可选）过渡期做一致性检查：
  - 在未删 FTQ 前，可以在 debug/assert 中对比 “FSQ 推导出的范围/分支点/target” 与 “FTQ head” 是否一致，帮助尽早发现不变量被破坏的情况。

**验收**
- fetch 不再依赖 `ftqHead()/ftqHasHead()`（或者这些接口已经等价转发到 FSQ）。
- squash/commit/update 训练路径保持可用。

---

### Phase 3：彻底删除 FTQ（删除类 + 删除构建源 + 删除相关逻辑）
**删除内容**
- `src/cpu/pred/btb/fetch_target_queue.hh`
- `src/cpu/pred/btb/fetch_target_queue.cc`
- `src/cpu/pred/SConscript`：删除 `Source('btb/fetch_target_queue.cc')`
- 清理所有 include/引用点：
  - `src/cpu/pred/btb/decoupled_bpred.hh`
  - `src/cpu/pred/btb/decoupled_bpred.cc`
  - `src/cpu/pred/btb/test/*`（涉及 FTQ 的测试需要改写或移除）

**BPU 行为变化**
- 删除 `tryEnqFetchTarget()` 整条路径（不再存在 FTQ enqueue state）。
- 明确 “fetch head 的 FSQ 指针”：
  - 增加 `FetchStreamId fetchHeadFsqId`（或者如果 head 永远等于 deque front，可直接从 `baseId` 推导）。
- `consumeFetchTarget(ftq_id, fsq_id, fetched_inst_num)`：
  - assert `ftq_id == fsq_id - 1`
  - 写回 `FetchStream.fetchInstNum = fetched_inst_num`
  - 推进 head（如 head 独立）或按队列语义消耗
- squash 处理：
  - 移除 `fetchTargetQueue.squash(...)`
  - 设置 `fetchHeadFsqId = stream_id + 1`（并对 FSQ 做 `eraseAfter(stream_id)`）

**性能/时序说明**
- 删除 FTQ 后，FSQ entry 可能会更早对 fetch 可见（甚至同拍可见）。
- 这是为了贴近 RTL 的“单队列”模型；如果需要保留 1-cycle 延迟，应在其他位置单独补回（不在本阶段强行制造额外状态机）。

**验收**
- BTB predictor 中不再存在任何 FTQ 符号/文件。
- fetch 完全由 FSQ head stream 驱动。

---

### Phase 4（后续）：清理/拆分 `FetchStream` 字段（减少“迷惑性字段”）
等 Phase 1–3 稳定后，再逐步做语义级清理：
- 可能的拆分方式：
  - `PredInfo`：start/end、taken、branch info、btb entries、metas、history snapshots
  - `ResolveInfo/UpdateInfo`：exeTaken/exeBranchInfo/updateBTBEntries/squash info
  - `Stats/Trace`：可选，必要时用开关控制
- 删除不再被 update/trace/统计路径使用的字段。

之所以放最后，是因为它会触及大量逻辑路径，属于“语义重构”，不适合和 FTQ 删除叠加在同一个阶段里做。

## 预计会改动的文件
- `src/cpu/pred/btb/decoupled_bpred.hh`
- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/o3/fetch.cc`
- `src/cpu/pred/SConscript`
-（删除）`src/cpu/pred/btb/fetch_target_queue.hh`, `src/cpu/pred/btb/fetch_target_queue.cc`
- `src/cpu/pred/btb/test/` 下相关测试（视情况）

## 验证清单
- 编译：
  - `scons build/RISCV/gem5.opt -j`
- 最小运行：
  - 任意能覆盖 branch + squash 的短 workload（按你本地已有脚本/配置即可）。
- 断言检查：
  - 消费 head stream 时，`pc` 始终满足 `[stream.startPC, stream.predEndPC)`。
  - 所有同时出现 `ftqId/fsqId` 的接口上，满足 `ftqId == fsqId - 1`。
