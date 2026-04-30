# Add FDIP (FTQ-directed ICache prefetch) for decoupled frontend

## Goal
Imported from OpenSpec change `add-fdip-icache-prefetch` at `.worktrees/fdip-phase2-xsdev/openspec/changes/add-fdip-icache-prefetch`.
在当前 decoupled frontend 中，Fetch 以 FTQ 为取指驱动（每个 FTQ entry 对应一次取指窗口），
且在实现了 `update-decoupled-btb-control-pc-views` 之后，当前仓库的 demand fetch request span
已按 **实际 coverage window** 计算：仍保留 66B fetch buffer 作为上限/容量，但 request span
不再隐式固定为 66B；对于跨边界 4B RVI control，窗口还可能因 control-tail coverage 被自然延长到完整指令尾部。
在这一语义下，ICache miss 仍然会直接导致前端停顿。

另外，本仓库的建模目标是尽可能贴近 XiangShan RTL 的前端/ICache 行为。XiangShan 的 FDIP/ICache
在 micro-arch 上具有一些关键语义（例如 `prefetchPtr/fetchPtr`、单周期 1 条预取仲裁、ITLB/PMP/mmio
过滤、redirect/`fence.i` 下丢弃 refill 避免污染）。若 OpenSpec 的 FDIP 方案与这些语义偏离过大，
后续在 `xs-dev` 主线落地会更难对齐与复现研究结论。

## Requirements
本变更保持 FDIP 大框架不变，但**本轮实现范围**收敛到优先级最高的 `P0` 项：

- **P0.1: negative-feedback suppress**
  - 为 FDIP 增加“最近一次生命周期为 `unused` 的 line 抑制”能力；
  - 目标不是追求“更像论文”，而是先压制当前统计上最强的负面信号：
    `AfterUnusedThenUnused`。
- **P0.2: old-path refill drop / install-side gating**
  - 将 old-path FDIP refill drop 作为本轮默认研究配置；
  - 至少保证 epoch-mismatched FDIP refill 不继续污染 L1I。

其余更重的结构（如 PIQ、prefetch buffer、完整 RTL tag/data 两段式接口）仍保留在 proposal 中，
但不属于本轮提交范围。

保留并对齐的基础框架包括：

1. **RTL-aligned 的 ICache 预取引擎（FDIP engine）**
   - 位置：O3 Fetch（decoupled frontend 路径）。
   - 行为：基于 FTQ 的 **`prefetchPtr/fetchPtr` 语义** 驱动预取：
     - 在 predictor/FTQ 内维护独立的 `prefetchPtr`（复位/redirect 对齐 `fetchPtr`）；
     - FDIP 每次只处理 `prefetchPtr` 指向的一个 FTQ entry，并按该 entry 的 **实际 fetch coverage window**
       （而非固定 66B overfetch 假设）向 ICache 发起 cacheline 粒度 inst-fetch 预取；
     - 默认 **每周期最多发出 1 条 cacheline 预取**（双行覆盖时分两拍），以贴近 RTL 的仲裁约束；
     - 仅当该 FTQ entry 所需 cacheline 都“已发出”或“被过滤”（fault/uncacheable/mmio）时，`prefetchPtr++`。
   - 约束：对每周期带宽、最大 outstanding、队列大小做上限控制；在资源不足时丢弃/推迟预取，
     **不得阻塞 demand fetch**（KISS：prefetch 不能影响正确性路径）。
   - squash/redirect：通过 epoch（或等价机制）标识生命周期，squash 后旧 epoch 的 FDIP
     outstanding/bytes 必须被丢弃或忽略，避免跨生命周期误用。
   - 可选（RTL-aligned）：提供一个“redirect/flush 下不安装 old-path refill”模式，用于更贴近
     XiangShan ICache 的冲刷语义；默认关闭以保持最小侵入与向后兼容。

2. **最小化的 FTQ peek API（供 FDIP/后续 BTB-entry prefetch 复用）**
   - 为 decoupled predictor（至少 `DecoupledBPUWithBTB`）提供“按 offset 读取 FTQ entry”
     的只读接口，不改变 FTQ 供给状态（不 pop / 不 advance）。
   - 目的：避免 FDIP 通过侵入式方式访问 predictor 内部队列，保持 SRP/OCP。

3. **参数、统计与文档**
   - 增加 enable/limit 参数（默认全关闭）。
   - 增加关键 stats（issued/dropped/useful/squashed 等）。
   - 文档说明：启用方式与参数语义。

## Acceptance Criteria
1. **默认不变**：FDIP 默认关闭时，功能/性能/统计与现状一致。
2. **不影响正确性路径**：FDIP 开启后，在资源不足时可以丢弃/推迟预取，但不得阻塞 demand fetch。
3. **squash 安全**：squash/redirect 后，FDIP 不会把旧路径产生的状态误用于新路径（通过 epoch 等机制保证）。
4. **可观测性**：提供最小但足够的统计，能定位“预取发出/被丢弃/被 squash/命中带来收益”等。
5. **coverage 对齐**：在 post-`control-pc-view` 基线上，FDIP 必须以每个 FTQ entry 的实际 fetch request
   coverage（`[startPC, predEndPC)` 及其自然延伸）为准；跨边界 4B control tail 的第二条线不能依赖旧式固定 66B overfetch 才被预取。
6. **P0.1 生效**：在至少一个高 I$ 压力 trace 上，`AfterUnusedThenUnused` 相比已有数据下降，且 `AfterUnusedThenUseful` 不发生灾难性退化。
7. **P0.2 生效**：old-path FDIP refill 在启用配置下不再安装到 ICache，并能在统计中被观测到。

## Technical Notes
- Source workspace: `fdip-phase2-xsdev`
- Source OpenSpec root: `.worktrees/fdip-phase2-xsdev/openspec`
- Source change dir: `.worktrees/fdip-phase2-xsdev/openspec/changes/add-fdip-icache-prefetch`
- Imported checklist progress: 38/116

## Impact Notes
- `src/cpu/o3/fetch.*`：新增 FDIP engine 的调度点与 squash 生命周期管理。
- `src/cpu/pred/btb/decoupled_bpred.*`：新增 FTQ peek + `prefetchPtr`（prefetch head）及 redirect 对齐语义。
- `src/mem/*`：必要时为 inst-fetch prefetch 请求增加最小标记/区分（SoftPFReq 等）；可选地增加 “old-path refill drop” 支持。
- 配置脚本与文档：新增参数说明与示例。
