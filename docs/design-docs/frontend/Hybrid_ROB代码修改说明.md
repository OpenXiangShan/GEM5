# Hybrid ROB 压缩代码修改说明

本文说明 2026-09-20 的 former/latter entry 实现；初版 b2088a2 的指令配额已不适用于 Hybrid。附件只作为背景资料，不是新的修改指令。

## 1. 模型单位与配置

DynInst 表示一条动态指令；融合 DynInst 可能对应两条架构指令。ROB 保持原有扁平指令链表，执行、寄存器更新、异常和 difftest 仍逐条进行。物理容量以 entry 为单位，一个 entry 可以包含多个 DynInst。

标准入口为 `configs/example/kmhv3.py`：

```sh
--param="system.cpu[0].RobCompressPolicy='hybrid'"
```

内层引号必须保留，因为 gem5 将 `--param` 作为 Python 表达式求值。Hybrid 自动设置单线程、关闭 value predictor、Load fusion 和 Rename 消除，`renameWidth=8`、`CROB_instPerGroup=8`、`commitWidth=8`。物理 ROB 容量沿用 352 entries。

`commitWidth` 在 Hybrid 下限制每拍处理的物理 entry 数。独立 DynInst 提交配额的参数、判断和满额统计已整体删除；非 Hybrid 保留原有 group-window 默认行为。默认非 Hybrid 策略没有因此切换。

## 2. 入队计划与持久状态

`rob.hh` 中 `HybridGroup` 保存 `formerLength`、`latterLength` 和规划类型，`memberCount()` 返回两者之和。规划类型仍区分 `NormalS/NormalC/NormalN` 以支持分类和分配统计；运行期 `HybridEntryType` 只有 `NORMAL/CC/CS/SC`。

`classifyHybridInst()` 保留原来的 N > C > S 分类。N 单独占 entry；S 序列压缩成一个 slot；C 单独占一个 slot。现有流式配对规则及 Rename 窗口边界不变：

| 输入 | former / latter | 运行期类型 |
|---|---|---|
| SSSS | SSSS / 空 | NORMAL |
| CSSS | C / SSS | CS |
| SSSC | SSS / C | SC |
| CC | C / C | CC |
| SSSCSS | SSS / C；SS / 空 | SC；NORMAL |

`planHybridBatch()` 只规划，不改 ROB。Commit 用 `plan.size()` 检查容量，`insertHybridBatch()` 消费同一份计划。失败批次不消耗 fixedbuffer，成功后才分配 entry 和记录成员元数据。

每个 `HybridEntryState` 保存 id、运行期类型、formerRemaining、latterRemaining。`DynInst` 增加 `hybridEntryId` 和 `hybridSlotIsFormer`。剩余数量随实际成员增删更新，不能用原始计划长度替代。

`hybridEntries` 是 Hybrid 物理容量的唯一依据，`threadGroups` 仅供旧策略使用。`getThreadEntries()`、`canAllocate()`、`numFreeEntries()`、满空判断和共享容量查询统一按策略读取。初始化清理 entry 和 squash 状态。

## 3. 提交路径

`Commit::commitInsts()` 在 Hybrid 下以 entry 完成为预算单位，不再把 group 窗口展开为普通指令配额。同一 entry 的成员按序执行原有 `commitHead()` 及其后续架构动作，最后一名成员移除后才释放物理容量。

队头 entry 选择时检查其成员就绪状态；后续成员仍必须 readyToCommit。原有 fault、非投机和严格有序访问处理路径保留。异常或 squash-after 可以使 entry 只完成部分架构提交，此时剩余成员继续占 entry，下一拍或恢复路径再处理；不会撤销已经提交的成员。

正常完成和 squashed-head draining 分开统计，但 draining 也不能突破每拍物理 entry 宽度。DynInst 分布上限由 `commitWidth * CROB_instPerGroup` 给出，不硬编码 8 倍。

## 4. Slot-aware squash 与降级

`HybridSquashTarget` 携带 entryId、slotIsFormer、flushItself：

| 目标 slot | flushItself | 本 entry 保留内容 |
|---|---|---|
| former | true | 无 |
| former | false | former |
| latter | true | former |
| latter | false | former 和 latter |

所有 younger entries 都被清除。`ROB::squashHybrid()` 同时生成最后保留指令的序号，Commit 将同一边界交给 Rename、IQ 和 LSQ。`doSquash()` 和恢复工作量计算采用一致的成员选择；NaiveCpt 使用归一化后的边界计算恢复代价。

当最后一个 latter 成员被 squash，且目标保留非空 former，`removeMember()` 将 entry 降级为 NORMAL。id 不变，不重配对。整 entry 清空不算降级；former 已经全部架构提交时，也不会把 latter 重新标成 former。

trap、TC 和已退休指令的 squash-after 继续使用 `squashAll()` 的架构序号边界，允许异常出现在部分退休的 S slot 内部。普通 IEW redirect 在序号减一前查找原始目标；Commit 检出的访存顺序冲突也进入 slot-aware 路径。

## 5. 统计、复杂度与验证

复用分配 entry 数、DynInst 数、类型／长度分布和压缩率。新增：

- `rob.hybridDowngrades`：保留 former 的降级次数。
- `commit.hybridCommittedEntries`：正常退休释放的 entry 数。
- `commit.hybridDrainedEntries`：squashed-head draining 释放的 entry 数。

ROB trace 记录分配 id、成员 slot、移除原因、保留边界及降级，可结合分配和退休统计检查物理宽度、容量、恢复边界和成员守恒。

规划 O(renameWidth)，成员增删 O(1)。正常提交受 `commitWidth * CROB_instPerGroup` 限制；redirect 初始化最多扫描有界 ROB，逐拍 squash 沿用原有恢复宽度。完整不变量扫描只在 ROB debug 打开时执行。

构建和验证命令见 `src/cpu/o3/README.hybrid.md`。GTest 覆盖 planner 穷举、四种 slot squash、跨拍 latter 清除和部分提交；文档另提供 CoreMark/NEMU 全系统验证命令。

## 6. 建模边界

本改动明确建模 entry 占用、提交带宽和 slot 恢复选择，没有引入 RAB、值预测、向量状态、flag tracker 或新流水级。内部数据通路和每拍信号仍采用 gem5 原有抽象，不声称与 RTL 周期级等价。SPEC 大规模性能测试和专项中断压力验证不属于此次结果。

## 7. 性能 CI 入口补充

[manual-perf.yml](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/.github/workflows/manual-perf.yml) 使用现有的 `kmhv3.py` 配置选项；在 `extra_args` 中通过 --param 选择 Hybrid：

    --param="system.cpu[0].RobCompressPolicy='hybrid'"

kmhv3.py 检测到这个参数后，会自动设置 Hybrid 所需的单线程、关闭 value predictor/Load fusion/Rename 消除、group 大小和提交参数；后续显式的 --param 设置仍可覆盖这些默认值。

[gem5-perf-template.yml](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/.github/workflows/gem5-perf-template.yml) 增加了 --param 参数的启动前检查。若输入被截断为 --param=system.cpu[0].numThrea 这类没有“参数值”的字符串，CI 会在启动 workload 前立即报出格式错误；否则每个 checkpoint 都会启动一次 gem5，再分别产生 KeyError，导致大量无关的 abort 文件。
