## 📄 文档：现代 TAGE 预测器设计：从流水线开销到块预测仲裁

本文档总结了将 TAGE 分支预测器从传统的“单分支/周期”模型适配到现代高性能 CPU“多分支/周期”块预测架构时所面临的关键设计挑战与解决方案。

### 1. 核心挑战：流水线开销与“更新时重读”

传统的 TAGE 预测器在“预测”阶段（Fetch）确定 `main` 和 `alt` provider，并将这些元数据（metadata）通过流水线寄存器传递到“更新”阶段（Commit）。

* **问题**：这种方法会消耗大量的管线寄存器面积，并且可能引入时序（timing）压力。
* **解决方案：“更新时重读”（Re-read on Update）**
    1.  **预测阶段**：正常查询 TAGE，得到预测结果。**不**保存 `main`/`alt` provider 的信息。
    2.  **更新阶段**：当分支结果（Commit）传来时，使用该分支的 `startAddr` 和 GHR **重新执行一次完整的 TAGE 查询**。
    3.  **重构**：通过并行的优先级编码器（Priority Encoders），在更新当拍动态地重新找出 `main`（最高命中表）和 `alt`（次高命中表）。
    4.  **更新**：只更新这两个表的 `ctr` 和 `u` bit，完全遵循标准 TAGE 算法。

* **优点**：极大简化了 CPU 流水线，移除了 TAGE 元数据。
* **缺点**：引入了新的硬件成本，见下一节。

### 2. 硬件挑战：SRAM 端口冲突与解决方案

“更新时重读”方案在 Commit 阶段引入了 TAGE SRAM 的“读”操作，这会与 Fetch 阶段的“预测读”在同一周期内发生冲突。

* **冲突**：Fetch（关键路径）和 Update（非关键路径）可能同时需要读取同一个 SRAM 资源。
* **解决方案：Bank 划分 + 仲裁 + 缓冲**
    1.  **SRAM 分组（Banked SRAM）**：将 TAGE 表（尤其是大表）物理上拆分为多个 Bank（例如 4-way）。Fetch 和 Update 访问同一张逻辑表时，有很大概率（如 75%）访问到不同的物理 Bank，从而避免冲突。
    2.  **仲裁（Arbitration）**：在每个 Bank 端口设置一个仲裁器。
    3.  **Fetch 优先（Fetch Priority）**：仲裁逻辑**永远**优先满足 Fetch 阶段的请求，因为它在 CPU 性能关键路径上。
    4.  **更新缓冲（Update Buffering）**：当 Update 请求因冲突（或 Bank 忙）而失败时，将该更新任务（PC, GHR, 结果）推入一个小的“更新队列”（Update Queue/FIFO）。一个独立的状态机在后续周期中不断尝试清空此队列。

### 3. 算法挑战：实现“块预测”的正确逻辑

在 BTB 一拍给出 Fetch Block 中多条分支（例如 8 条）的架构下，TAGE 的目标是**预测出这个块中“第一个 Taken”的分支**，并在其后截断。

* **错误模型 1（表分区）**：将 TAGE 表 T1-T8 分配给 B1-B8。**错误**，这破坏了 TAGE 的长短历史特性。
* **错误模型 2（单点赢家）**：用 `startAddr` 查所有表，命中的最高表（如 $T_7$）“赢家通吃”，它提供的 `{pos, dir}` 作为唯一预测。**错误**，如下例所示，它无法处理位置仲裁：
    * $T_7$ 预测 `{pos=3, dir=Taken}`
    * $T_5$ 预测 `{pos=1, dir=Taken}`
    * 此模型会错误地选择 $T_7$ 的预测（B3 Taken），而忽略了 B1 才是第一个 Taken。

* **正确模型：“分组-再排序”（Group-then-Sort）**
    1.  **并行查询**：使用 `startAddr` + GHR，查询所有 TAGE 表（T1...TN），读出所有命中的表项。
    2.  **按 Position 分组**：在逻辑上，为 Fetch Block 中的每一个分支（B0, B1, ... BN）聚合它自己的“命中集”。
        * `B1_HitSet` = {所有命中且 `entry.pos == 1` 的表}
        * `B3_HitSet` = {所有命中且 `entry.pos == 3` 的表}
    3.  **Per-Branch 预测**：为每个分支独立地从其 `HitSet` 中选出 `main` 和 `alt`，得出一个预测方向。
        * `B1_Prediction` = `main(B1_HitSet).dir`
        * `B3_Prediction` = `main(B3_HitSet).dir`
    4.  **位置排序仲裁**：从 B0 开始，依次检查每个分支的预测结果。**第一个**被预测为 `Taken` 的分支成为最终的截断点。

### 4. 关键问题：“块内别名”及其解决方案

上述“正确模型”引入了一个新的、微妙的性能问题：**块内别名（Intra-Block Aliasing）**。

* **问题**：
    * TAGE 表的 `index` 和 `tag` 是由 `(startAddr, GHR)` 计算的。
    * `position` 字段只是存储在表项中的**数据荷载（Payload）**，不参与索引。
    * **冲突**：当 B1（pos=1）和 B3（pos=3）在*相同*的 `(startAddr, GHR)` 上下文中都需要一个高表（如 $T_7$）的条目时，它们会哈希到**同一个** `index`，从而竞争**同一个**表项槽位。
    * **后果**：如果 B3 先占据了 `T7[Index_X]`，那么 B1 在发生错误预测后将无法分配到 $T_7$ 中，导致 B1 的预测精度持续低下。

* **解决方案：N路组相连（N-Way Set-Associativity）**
    * 将 TAGE 表（尤其是高表）实现为 N 路组相连（例如 2-Way 或 4-Way）。
    * 当 B1 和 B3 哈希到同一个 `Index_X` 时，B3 可以占据 `Set[Index_X][Way 0]`，而 B1 可以被分配到 `Set[Index_X][Way 1]`。
    * 这显著缓解了由块预测引入的“块内别名”问题，允许同一 Fetch Block 中的多个“困难”分支共享高表资源。

### 5. 落地方案：更新时重读（Re-read on Update）实现计划

为降低流水线寄存器压力、去除预测阶段对 main/alt 元数据的跨拍传递，本实现在更新阶段“重读”TAGE，并只更新到停止点（StopPoint）。

- 变更目标
  - 预测期：正常查表与产出方向，但不再依赖在更新期使用的 main/alt 元数据。
  - 更新期：基于预测当拍的折叠历史快照（PHR/GHR 折叠值）重读所有表，逐分支还原 main/alt 与 use_alt_on_na gating，执行更新与分配。
  - 只更新到停止点：沿用 `selectPredictedBTBEntriesForUpdate` 的裁剪行为，B0…StopPoint 分支被更新，StopPoint 之后不更新。

- 核心改造点
  1) 新增“基于快照重读 provider”的辅助函数（以单分支为粒度）：
     - 输入：`alignedPC`、`BTBEntry`、`TageMeta`（仅使用其中的 `indexFoldedHist/tagFoldedHist/altTagFoldedHist` 快照）。
     - 对每张表 `i`：用快照计算 `index_i/tag_i`，在 `tageTable[i][index_i][way]` 中匹配 `{valid && tag==tag_i && pc==entry.pc}`，按“表号从高到低”选 main、alt。
     - 结合 base 表与 `use_alt_on_na` 得到最终预测（与预测期逻辑一致）。
     - 更新快照中的 `hitWay/hitFound`（供 useful 重置使用）。

  2) 新增“基于快照读取 usefulMask”的辅助函数：
     - 用快照 index 访问每张表的该 set，按 way 采集 `useful>0` 位，写入 `TageMeta::usefulMask[way][table]`。
     - 后续 `handleUsefulBitReset` 与 `handleNewEntryAllocation` 使用该 mask，避免历史不一致。

  3) 改造 `update()`：
     - 以 `stream.predMetas[idx]` 的 `TageMeta` 作为更新期唯一快照，并赋给类成员 `meta`，保证辅助函数与重置逻辑使用同一份数据。
     - 遍历 prepared update entries 中的分支（已裁剪到停止点），对每个分支：
       * 用“重读 provider”获得 `TagePrediction`；
       * 走原有 `updatePredictorStateAndCheckAllocation` 更新计数器与 `use_alt_on_na`；
       * 若需要分配：用“快照 usefulMask”→`handleUsefulBitReset`→从 `main.table+1` 起调用 `handleNewEntryAllocation`；
     - 统计与 trace 使用重读得到的 main/alt 信息（必要处做 `found` 判断保护）。

  4) 向后兼容与最小侵入
     - 不移除原有 `generateSinglePrediction`，仅在 `update()` 中改用“重读 provider”。
     - 其他接口与替换/分配策略保持不变；仅纠正 `recordUsefulMask` 在更新期的历史来源，使之基于快照。

- 细节注意
  - 历史一致性：更新期一律以 `TageMeta` 快照计算 index/tag，不使用对象内随推测更新滚动的 folded hist。
  - 分配冲突：同一更新周期多分支竞争同 set，按遍历顺序“位置靠前优先”；多路组相连（`numWays`）本身可缓解冲突。
  - 统计与调试：`tageStats.updateStatsWithTagePrediction(pred,false)` 与 DB trace 字段改用重读结果，避免统计混入预测期缓存数据。
