# 向量配置指令在 Decode stall, 指令执行完毕解除

## 1. 背景与问题

目标：

- decode 识别到向量配置指令后，立即建立执行屏障；
- 向量配置指令执行完成前，不允许其后续指令继续执行；
- 若 ROB 中存在该指令之后且已经执行的指令，进行刷新重放。

## 2. 设计原则

- **最小侵入**：优先复用 O3 既有的 `SerializeBefore/SerializeAfter` 与 `squashAfter` 机制。
- **语义优先**：先保证顺序正确，再考虑性能微优化。
- **兜底保护**：即使存在历史在飞窗口，也通过 commit 检测触发刷新收敛到正确状态。

## 3. 修改点与代码级方案

### 3.1 `src/cpu/o3/decode.cc`

在 `Decode::decodeInsts(ThreadID tid)` 中，当遇到 `inst->staticInst->isVectorConfig()` 时：

1. 对该 `DynInst` 设置：
   - `inst->setSerializeBefore();`
   - `inst->setSerializeAfter();`
2. 设置本轮 decode 终止原因为 `StallReason::SerializeStall`；
3. 立即停止继续 decode 后续指令（避免同轮将 younger 指令继续推送到 rename/IEW）。

效果：

- `SerializeBefore`：该向量配置指令在 IEW 会按序列化语义等待；
- `SerializeAfter`：该指令一旦可派发，自动对后继建立序列化阻塞；
- 同轮 decode 截断可进一步减少“后继先行”窗口。

### 3.2 `src/cpu/o3/commit.hh` + `src/cpu/o3/commit.cc`

新增 commit 兜底检测函数（私有）：

- `bool hasExecutedYoungerInst(ThreadID tid, InstSeqNum seq_num) const;`

实现逻辑：遍历 `rob->getInstList(tid)`，若发现 `seqNum > seq_num` 且 `isExecuted()` 且未 squashed 的指令，则返回 true。

在提交循环中，当 `head_inst->staticInst->isVectorConfig()` 且检测命中时：

1. 调用 `squashAfter(tid, head_inst);`
2. 触发对后续在飞 younger 指令的刷新重放。

效果：

- 正常路径下几乎不触发；
- 异常窗口下确保最终收敛到“向量配置之后指令需在其完成后再执行”的语义。

## 4. 时序说明（简化）

1. Decode 看到 `vset*`（向量配置）→ 打序列化标记并停止同轮继续下推；
2. IEW 依据 serialize 规则阻塞后续派发；
3. 向量配置执行并提交；
4. 若提交时检测到 younger 已执行 → `squashAfter` 刷新重放；
5. 向量配置完成后，后续指令恢复执行。

## 5. 风险与规避

- **性能风险**：序列化会增加停顿；该行为符合 fence-like 目标。
- **过度刷新风险**：commit 仅在“向量配置提交点 + younger 已执行”时触发，降低误触发概率。
- **功能回归风险**：保持现有 `fetch waitForVsetvl` 与 `commit vtype 更新` 不变，避免破坏既有路径。

## 6. 验证计划

### 6.1 编译验证（必须）

- `scons build/RISCV/gem5.opt --gold-linker -j64`

### 6.2 行为验证（建议）

- 运行含 `vsetvl/vsetvli` 的最小 workload；
- 打开相关 DPRINTF（`Decode/IEW/Commit`）确认：
  - decode 命中向量配置后出现 serialize stall；
  - 向量配置完成前无 younger 继续执行；
  - 若出现 younger 已执行窗口，会触发 squash。

## 7. 影响范围

- `src/cpu/o3/decode.cc`
- `src/cpu/o3/commit.hh`
- `src/cpu/o3/commit.cc`
- 文档：`docs/exec-plans/active/vector-config-decode-blocking.md`
