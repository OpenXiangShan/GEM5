# Add FDIP (FTQ-directed ICache prefetch) for decoupled frontend

## Goal
Imported from OpenSpec change `add-fdip-icache-prefetch` at `.worktrees/fdip-rtl-align-xsdev/openspec/changes/add-fdip-icache-prefetch`.
当前 worktree 已经落了一版 **默认关闭** 的 FDIP baseline：Fetch 能按 FTQ 的
`prefetchPtr` 发起 inst-fetch prefetch，cache 侧也已经有 FDIP provenance、
L1I `useful/late/unused` 统计，以及可选的 old-path refill drop。这个 baseline
已经足够作为后续优化的稳定起点，但它还不是 RTL 对齐版本。

XiangShan RTL master 当前真正关键的 FDIP/ICache 语义不是“多发几个 prefetch”，而是：
- FTQ 同时维护独立的 `pfPtr/ifuPtr`
- prefetch 先过 **TLB + meta/tag**，只有 `!hit && !exception && !mmio`
  才进入 miss path
- fetch / prefetch miss 资源分池（`nFetchMshr=4`, `nPrefetchMshr=10`）
- fetch 始终高优先级，prefetch 不直接挤占 demand fetch 的 miss 资源

当前 gem5 baseline 仍然把 FDIP miss 直接送进 L1I 的普通 miss 路径，并与 demand fetch
共享当前的 4 个 MSHR。因此当前观测到的 FDIP 负收益，很可能相当一部分来自
**shared-MSHR artifact** 和 **过早进入 miss path**，而不是 RTL 风格 FDIP 本身。

因此，本 change 现在的重点不是继续扩大 FDIP 发射范围，而是把这条 baseline 收敛到
RTL 的关键外部语义：**资源隔离、gate-before-miss、duplicate suppression**，
以及在确有必要时才引入更像 RTL 的轻量 tag-side hint。

另外，FDIP proposal 仍然依赖一个更基础的前提：decoupled BTB / fetch 必须先明确
4B RVI control-flow instruction 在跨 predict/fetch block 边界时的 trigger/range 语义。
也就是说，只有在 `update-decoupled-btb-control-pc-views` 把
`startPC / triggerPC / endPCExclusive` 语义锁定后，FDIP 才能稳定定义
“一个 FTQ entry 需要覆盖哪些 cacheline”以及 `prefetch_lines_per_ftq` 的研究口径。

## Requirements
本变更现在按 **baseline 已落地 + RTL 对齐后续 phase** 的方式推进。

1. **Phase 0 / 已落地 baseline**
   - predictor/FTQ 内维护独立的 `prefetchPtr`
   - Fetch 以 cacheline 粒度驱动 FDIP，支持双行覆盖、issue bandwidth、outstanding limit
   - prefetch 请求携带 FDIP provenance / epoch / FTQ id / startPC
   - cache 侧已有 `fdipInstalled/usefulHits/late/unused/epochMismatch/droppedRefill`
   - 可选 `fdip_drop_refill_on_epoch_mismatch`

2. **Phase 1 / 必做：去掉 shared-MSHR artifact**
   - 给 L1I 加入 **demand / FDIP miss 资源隔离**，最小目标是能表达
     XiangShan 风格的 `fetchMSHR=4`、`prefetchMSHR=10`（或等价 quota）
   - demand fetch 必须保持高优先级，FDIP 不能耗尽 demand 的 miss capacity
   - 这是当前 RTL 对齐路线的第一优先级

3. **Phase 1 / 必做：gate-before-miss**
   - 在 FDIP 进入 miss allocation 之前先做非分配式的 L1I probe/tag gate
   - hit / duplicate / in-flight same-line miss 时，不再创建新的 FDIP miss
   - 这样才能更接近 RTL 的 “TLB+meta 命中后再决定是否进 MissUnit”

4. **Phase 2 / 推荐：duplicate suppression + regression closure**
   - 补足更稳定的 duplicate suppression 与 per-line merge 语义
   - 用 66 条 icache-sensitive trace 做 `off / on / on+drop` 对照
   - 输出能区分“timeliness 收益”和“contention 代价”的结果表

5. **Phase 3 / 可选：轻量 tag-side hint / shadow WayLookup**
   - 只有在 phase-1/2 之后仍确认与 RTL 差距主要来自 tag/data 解耦时才做
   - 不在第一轮直接 full clone RTL ICache pipeline

6. **继续保持独立 proposal 的内容**
   - `add-fdip-byte-window-provider`
   - `add-btb-entry-prefetch`
   - 它们都不是当前这轮 RTL 对齐必须先做的步骤

## Acceptance Criteria
1. **默认不变**：FDIP 默认关闭时，功能/性能/统计与现状一致。
2. **不影响正确性路径**：FDIP 开启后，在资源不足时可以丢弃/推迟预取，但不得阻塞 demand fetch。
3. **squash 安全**：squash/redirect 后，FDIP 不会把旧路径产生的状态误用于新路径（通过 epoch 等机制保证）。
4. **资源隔离**：FDIP 不再与 demand fetch 无保护地共享同一个 4-MSHR 池；必须存在 demand-protected quota
   或等价的 source-specific miss capacity。
5. **gate-before-miss**：FDIP 在 tag hit / duplicate / in-flight same-line miss 时不分配新的 prefetch miss 资源。
6. **可观测性**：统计必须能区分 timeliness 收益与 contention 代价，至少覆盖
   `issued/useful/late/unused/epochMismatch` 以及 phase-1 新增的 probe / quota 拒绝类计数。

## Technical Notes
- Source workspace: `fdip-rtl-align-xsdev`
- Source OpenSpec root: `.worktrees/fdip-rtl-align-xsdev/openspec`
- Source change dir: `.worktrees/fdip-rtl-align-xsdev/openspec/changes/add-fdip-icache-prefetch`
- Imported checklist progress: 34/52

## Impact Notes
- `src/cpu/o3/fetch.*`：baseline 已落地；后续主要配合 gate-before-miss / stats 对齐
- `src/cpu/pred/btb/decoupled_bpred.*`：baseline 已落地，保留 `prefetchPtr` / lookahead API
- `src/mem/cache/*`：后续 phase 的核心实现区，负责资源隔离、gate-before-miss、duplicate suppression
- `src/mem/request.hh`：继续承载 FDIP provenance 与 epoch metadata
- 配置脚本与 validation：补充 XiangShan 风格 quota 配置、CSV manifest 与对照脚本

## Validation Notes
本 change 的验证以
`openspec/changes/add-fdip-icache-prefetch/validation/tasks.csv`
为执行源，环境模板在
`openspec/changes/add-fdip-icache-prefetch/validation/env.example.sh`。

后续实现至少满足以下可直接执行步骤：

1. **Spec validation**
   - 在对应 worktree 根目录执行：
     - `openspec validate add-fdip-icache-prefetch --strict`

2. **Dependency gate**
   - 在开始 FDIP engine 主实现前，确认 control-PC/range 语义 proposal 已完成或其关键语义已被当前分支吸收。
   - 至少检查：
     - 跨块 4B RVI control-flow instruction 的 trigger 行为已明确
     - `prefetch_lines_per_ftq` 的窗口定义已锁定

3. **Build validation**
   - 执行：
     - `scons -j8 build/RISCV/gem5.opt`
     - `scons -j8 build/RISCV/cpu/pred/btb/test/fetch_target_queue.test.opt`
     - `build/RISCV/cpu/pred/btb/test/fetch_target_queue.test.opt`

4. **Three-way smoke**
   - 对同一条 champsim trace 分别执行：
     - FDIP off
     - FDIP on
     - FDIP on + `--fdip-drop-refill-on-epoch-mismatch`
   - 检查：
     - 都能跑到 `maxinsts`
     - on/off 对比中 demand path 不出现新增 fatal / assert
     - on/drop 对比中 old-path refill drop 统计可见

5. **Plumbing-level direct checks**
   - 对 peek / `prefetchPtr` 检查至少覆盖：
     - reset 后 `prefetchPtr == fetchPtr`
     - redirect/squash 后 `prefetchPtr` 对齐新 `fetchPtr`
     - out-of-range peek 返回可定义的空/失败语义

6. **66-trace icache-sensitive compare**
   - 跑 `off / on / on+drop` 三组 66 条 trace 的对照
   - 检查：
     - `fetch.icacheStallCycles` 的方向变化可解释
     - `fdipUsefulHits / fdipLate / fdipUnused / fdipDroppedRefill`
       能解释主要收益/回归来源
     - phase-1 完成后，probe/quota 统计能单独量化 shared-MSHR artifact 是否被消除
