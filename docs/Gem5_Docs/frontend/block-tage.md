本文档是一份偏 PRD/架构说明的设计稿，目标读者包括：

- 架构/RTL 同学：希望快速理解为什么要做、要做成什么语义、关键状态机如何跑。
- Gem5/模型同学：希望能直接映射到现有 DecoupledBTB/BTBTAGE 的接口与更新时序。

本文聚焦第一阶段：**Block-Based Exit-Slot TAGE（Cond Exit）**。Two-Taken 仅保留为后续扩展方向。

---

# 架构演进提案：基于 Block 粒度的 Exit-Slot TAGE（Cond Exit）与 Two-Taken 预测机制

## 1. 背景与动机 (Motivation)

在 Gem5/香山高性能核的 SPEC06 性能分析中，我们发现前端带宽（Instruction Delivery）在 8-wide 架构下存在显著瓶颈。现有的 BTB-TAGE 组合方式存在以下痛点：

1. **信息密度低 / 资源浪费**：当前做法是“Block 内每条 Cond 分支都单独预测方向”，但很多 Block 的真实行为往往是“最多只有一条 Cond Taken”。这会造成训练样本被稀释、表项被无效占用。
2. **同 Block 多分支的 Set 压力与互相污染**：当前索引主要由 `StartPC + PHR` 决定，同一个 Fetch Block 内多条分支会落在同一个 set（靠 tag 中 XOR 的 position 来区分）。在 way 数较小（如 2-way）时，多分支会互相挤占/替换，等价于你文档里想表达的“aliasing/冲突”问题（这不是传统意义的 bank conflict，而是 set-assoc 压力）。
3. **Two-Taken 缺失**：由于 BPU 内部 override 机制导致的流水线气泡，无法满足后端 8 发射的饥渴需求。（Two-Taken 本文先不展开实现细节）

**本提案旨在通过实现 "Block-Based Exit-Slot TAGE（Cond Exit）" 和 "Speculative Two-Taken" 机制，将 BPU 的有效吞吐提升至 >1 Block/Cycle。**

---

## 2. 核心架构设计 (Architecture Overview)

我们将 TAGE 从 **"Per-Branch Direction Predictor"** 重构为 **"Block-Based Cond-Exit Predictor"**：

- TAGE 只负责 **Cond 分支的“退出点”选择**（即：Block 内哪一个 Cond 分支会是第一条 Taken）。
- Uncond/Indirect/Return 的处理保持现有 BTB 流水线逻辑，不在本次改动范围内。

“Block-Based（Exit-Slot）” 相比“Per-Branch”的真正优势，不在于“多Pattern”时的容量，而在于“单Pattern”时的效率、抗干扰能力以及带宽匹配度。
目的是提升信息密度：你的方法输出的是一个向量 (Vector) [T/NT, T/NT, T/NT]；Exit-Slot 输出的是一个标量 (Scalar) slot 编码。对于单目标跳转体系，标量比向量更抗噪。

### 2.0 设计目标 / 非目标（PRD）

**目标（Goals）**

1. 将 Cond 分支方向预测从 “每条分支一个表项/一个预测” 转为 “每个 Fetch Block 一个 payload（ExitSlotEnc）”。
2. 保持与现有 GEM5 DecoupledBTB 的接口兼容：仍输出 `condTakens`，上层仍通过 “按 PC 顺序选择第一条 taken” 得到最终控制流出口。
3. 更新与分配以 “每个 Fetch Block 一次训练样本” 为粒度，避免对 exit 之后不可达 cond 分支做 NT 训练（减少系统性噪声）。

**非目标（Non-Goals）**

1. 不改变 Uncond/Indirect/Return 的预测与选择规则。
2. 第一阶段不引入 Two-Taken 的细节实现（但文档保留扩展点）。
3. 第一阶段不引入复杂的多 payload/向量输出（例如同时预测多个 cond 的 T/NT）。

### 2.1 概念定义（与现有 GEM5 BTB 模型对齐）

* **Fetch Block**: 取指块。当前 DecoupledBTB 模型里 `predictWidth = 64B`，并按 PC 顺序返回该范围内的 BTB entries。
* **Slot（指令位置槽）**：以 **2B 粒度**划分 64B block，共 `32` 个 slot，范围 `0..31`。slot 计算方式与当前实现一致：`slot = (branchPC - alignedStartPC) >> instShiftAmt`，其中 `instShiftAmt=1`。其中 `alignedStartPC` 取 fetch 起始地址按 32B 对齐（MBTB half-aligned），因此 slot 覆盖的地址范围是 `[alignedStartPC, alignedStartPC+64B)`。
* **Cond Exit Slot**：指示该 Fetch Block 内 **第一条 Taken 的 Cond 分支**位于哪个 slot。
* **No-Cond-Exit（本文仍沿用“fallthrough”术语）**：表示该 Fetch Block 内 **没有 Cond Taken**（注意：这不排除 block 内存在 Uncond/Indirect/Return 导致的控制流退出；本提案的 TAGE 只负责 Cond Exit）。

### 2.1.1 关键语义澄清（给 RTL/模型同学）

- “fallthrough / No-Cond-Exit” 在本文中仅表示 **cond 维度的 fallthrough**：即该 block 内没有 cond taken。
- 若 block 内存在 uncond/indirect/return，它们依然可能成为最终控制流出口；这不由 Exit-Slot TAGE 决定。

### 2.1.2 兼容现有接口的落地方式

现有框架最终通过扫描 `btbEntries` 并结合 `condTakens` 选出第一条 taken entry。为了最小改动：

- Exit-Slot TAGE 仍然生成 `condTakens`；
- 但不再为每条 cond 输出方向，而是 **最多只标记 1 条 cond taken**（对应预测的 exit slot）；其余 cond 默认不在 `condTakens` 中出现，等价 NT。



### 2.2 组件交互图（概念）

```text
[PC] ^ [PHR]
      |
      v
+------------------------+      +------------------------+
|   Block-Based TAGE     |      |    Auxiliary GShare    |
| (Main Predictor)       |      | (For 2nd Taken)        |
| Output: ExitSlot_1     |      | Output: Is_Taken_2?    |
+------------------------+      +------------------------+
      |                                  |
      +---------------+  +---------------+
                      |  |
                      v  v
+------------------------------------------------+
|               MBTB (Multi-Target BTB)          |
|  Lookup(PC1) -> { Branch_1..N_Info, Targets }  |
+------------------------------------------------+
                      |
                      v
            Final Decision Logic
    1. Taken 1: TAGE predicts ExitSlot_1 (Cond exit slot). 
       Then select the corresponding cond branch entry from MBTB entries (by slot) and mark it taken.
    2. Taken 2: If Taken 1 is Taken AND GShare says Taken:
       Get First_Branch Target from MBTB (Next Line Logic).

```
但在 GEM5 当前模型架构中，是先查 Main BTB 结构得到一个 block 内命中的 BTB entries（按 PC 顺序），再交给方向预测器填充 `condTakens`。本提案的第一阶段会保持接口兼容：仍然输出 `condTakens`，只是由 “per-branch” 改为 “per-block 选中一个 cond exit”。

### 2.3 设计约束与假设（Implementation Constraints）

- **slot 编码选择**：使用指令位置 slot（0..31），而非“第 N 条分支”。原因：slot 语义稳定，不随 MBTB 命中条目集合变化而漂移。
- **payload 编码**：由于 32 个 slot + 1 个 No-Cond-Exit，推荐使用 **6 bits 的 `ExitSlotEnc`**：
  - `ExitSlotEnc==0`：No-Cond-Exit
  - `ExitSlotEnc in [1..32]`：slot = ExitSlotEnc - 1
- **训练粒度**：每个 fetch block 只训练一次（围绕真实的 cond exit），不训练 exit 之后不可达 cond。
- **回退策略**：payload 不可映射（找不到该 slot 的 cond entry）时，优先回退到 base（MBTB entry 的 `ctr`）。
- **保持经验法则**：保留 useAltOnNa “provider 弱态时是否用 alt/base” 的机制；但索引从 branchPC 改为 startPC（block 粒度）。

---

## 3. 详细设计：Block-Based TAGE（Cond Exit / Taken 1）

### 3.1 表项结构 (Entry Structure)

不再存储 1-bit Direction，而是存储 “Cond Exit Slot（或 No-Cond-Exit）”。

> 关键点：64B block 有 32 个 slot（0..31），“No-Cond-Exit” 是额外的一个状态，因此**单独用 5 bits 无法同时表示全部 slot + fallthrough**。
>
> 推荐采用 **6 bits 编码**（或等价的 `5bits slot + 1bit is_fallthrough`）。

| Field | Bits | Description |
| --- | --- | --- |
| **Tag** | 8-16 | `Hash(StartPC, PHR)`，用于匹配 Block。 |
| **Conf** | 2-3 | **置信计数器（建议 3 bits）**：表示该 payload 在该相关历史下是否稳定可靠。<br>弱态阈值建议沿用现有经验：`Conf in {0, -1}` 视为 weak。<br>更新规则与 per-branch 的 taken/nt 不同：**用 “是否预测正确” 来更新 Conf**（见 3.3）。 |
| **ExitSlotEnc** | 6 | **Payload**（推荐编码）：<br>`0`: No-Cond-Exit（本文仍称 fallthrough）<br>`1..32`: 表示 `slot = ExitSlotEnc - 1`，范围 `0..31` |
| **U** | 1 | Useful bit，用于替换策略 (Clock/Ageing)。 |

**Conf 与 U 的分工（必须写清楚）**

- `Conf`：回答“这个 payload 在这个相关历史下是否稳定可靠”，主要用于 **useAlt 门控**、**防抖（是否允许 rewrite）**、以及 **是否值得 allocate 长历史**。
- `U`：回答“这条表项是否相对 alt/base 提供了增益”，主要用于 **替换/分配候选选择**（例如优先替换 `U==0` 的 entry）。

### 3.2 预测逻辑 (Prediction Stage)

本节描述 **预测阶段**在一个 fetch block 上的完整行为：如何从 TAGE 表项得到 `ExitSlotEnc`，以及如何将其落地到 `condTakens`。

#### 3.2.1 Index/Tag（与现有实现对齐）

1. **Index**：仅使用 `StartPC + FoldedPHR`（不加入 branch offset）。
2. **Tag**：使用 `StartPC + FoldedPHR`；无需再 XOR position（因为一个 block 只对应一个 payload）。

> 说明：现有 per-branch TAGE 的 tag 会 XOR position 来区分同一 block 内的多条分支；Exit-Slot TAGE 的目的正是把这些分支“压缩”为一个 block-level payload，因此不再需要 position 进入 tag。

#### 3.2.2 Provider/Alt 选择（最长历史优先）

- 从最长历史表向短历史表扫描命中：
  - 第一命中为 Provider
  - 第二命中为 Alt Provider（用于弱态/冲突时回退）

#### 3.2.3 useAltOnNa 门控（沿用经验，但索引换成 startPC）

- Provider miss：直接回退 Base。
- Provider hit 且 `Conf` 为 weak（建议 `Conf in {0,-1}`）：
  - 查询 `useAltOnNa[startPC]` 决定使用 Alt（若存在）或 Base；
- Provider hit 且 `Conf` 非 weak：使用 Provider payload。

#### 3.2.4 将 payload 落地为 `condTakens`（接口兼容的关键）

解码得到 `(is_no_cond_exit, pred_slot)`：

- 若 `ExitSlotEnc==0`：
  - 不写入任何 cond 的 taken（等价所有 cond NT）
- 若 `ExitSlotEnc in [1..32]`：
  1. 在 MBTB 返回的 `btbEntries` 中寻找 `isCond==true` 且 `slot(entry.pc)==pred_slot` 的 entry；
  2. 找到则仅写入这一条 `condTakens[entry.pc]=true`；
  3. 其余 cond entry 不写入 `condTakens`（等价 NT）。

**Fallback（payload 不可映射）**：

- 若找不到 `pred_slot` 对应的 cond entry（MBTB miss/过滤/未学到等）：
  - 回退 Base：对每条 cond entry 使用 MBTB 的 `ctr>=0` 作为方向预测，生成 `condTakens`；
  - 这是为了避免 “payload 不可映射 ⇒ 强制 No-Cond-Exit” 带来的不必要性能退化。

**Base 的精确定义（便于 RTL/模型一致）**

- 对每条 `btbEntries` 中的 cond entry：
  - `pred_taken = entry.alwaysTaken || (entry.ctr >= 0)`
  - 写入 `condTakens[entry.pc] = pred_taken`
- 若某条 cond entry 没写入 `condTakens`，上层会按 “未找到即视为 NT” 处理。

#### 3.2.5 伪代码（预测阶段）

```text
predict_block(startPC, btbEntries, PHR):
  provider, alt = tage_lookup(startPC, PHR)
  if provider.miss:
    return base_condTakens(btbEntries)

  if is_weak(provider.Conf) and useAltOnNa[startPC] says "use alt":
    if alt.hit:
      enc = alt.ExitSlotEnc
    else:
      return base_condTakens(btbEntries)
  else:
    enc = provider.ExitSlotEnc

  if enc == 0:
    return {}  // all cond NT

  pred_slot = enc - 1
  e = find_cond_entry_by_slot(btbEntries, pred_slot)
  if e.exists:
    return { e.pc : true }  // only one taken
  else:
    return base_condTakens(btbEntries)
```



### 3.3 更新逻辑 (Update Stage)

每个 Fetch Block **只更新/分配一次**，并且**不对 exit 之后的 cond 分支进行“NT 训练”**（它们在该动态 instance 中不可达）。

本节给出 **可直接给 RTL 同学实现** 的更新/分配状态机：什么时候只训练 Conf，什么时候 rewrite payload，什么时候 allocate 长历史表项。

#### 3.3.1 真实标签 `RealEnc` 的定义（Cond 维度）

- 若 `stream.exeTaken==true` 且 `stream.exeBranchInfo.isCond==true`：
  - `real_slot = slot(stream.exeBranchInfo.pc)`
  - `RealEnc = real_slot + 1`
- 否则：
  - `RealEnc = 0`（No-Cond-Exit）

> 说明：若最终出口是 uncond/indirect/return，本提案把 `RealEnc` 视为 0，因为 TAGE 只负责 cond exit。

#### 3.3.2 预测标签 `PredEnc` 的定义（与预测阶段保持一致）

更新时应使用“预测阶段最终生效的决策”来计算 `PredEnc`：

- 若最终使用了某个 TAGE provider/alt 的 payload：`PredEnc = ExitSlotEnc`
- 若走了 Base 回退：
  - 令 `PredEnc = base_exit_slot_enc(btbEntries)`：
    - 若 base 在该 block 内预测到某条 cond taken：`PredEnc = slot(pc_first_taken_cond)+1`
    - 否则：`PredEnc = 0`

其中 `base_exit_slot_enc` 的计算方式为：

1. 按 `btbEntries` 的 PC 顺序扫描 cond entry；
2. 对每条 cond 计算 `pred_taken = entry.alwaysTaken || (entry.ctr >= 0)`；
3. 返回第一条 `pred_taken==true` 的 cond 的 `slot(pc)+1`；若不存在则返回 0。

#### 3.3.3 Conf/U 的更新（正确性驱动，而非 taken/nt 驱动）

令 `correct = (PredEnc == RealEnc)`。

- 若 `correct`：
  - `Conf = sat_inc(Conf)`
  - `U`：当 **provider 被选用** 且 provider 正确，并且 alt/base 的结果会不同/更差时置 1（表示这条表项“提供了增益”）。一种可执行的定义是：\
    `provider_used && correct && (AltOrBasePredEnc != RealEnc)  =>  U=1`。
- 若 `!correct`：
  - `Conf = sat_dec(Conf)`
  - `U`：可在进入弱态时清 0（更保守），或直接清 0（更激进，利于替换）。

> 关键点：Conf 的更新以 “payload 是否正确” 为准；这与 per-branch TAGE 里 “按 taken/nt 更新 counter” 不同，是本 PRD 的核心变化之一。

#### 3.3.4 分配/重写策略（建议的三条硬规则）

为兼顾收敛速度与稳定性，推荐采用下述三条规则：

1. **weak-but-correct：不分配**
   - 若 provider hit，且 `is_weak(Conf)`，但 `correct==true`：
   - 只训练 `Conf++`（“还不够自信，继续训练”），不 allocate 长历史表，避免浪费与 ping-pong。

2. **strong-but-wrong：倾向分配长历史表项**
   - 若 provider hit，且错误发生前 `Conf` 为 strong（非 weak 且接近饱和），但 `correct==false`：
   - 解释：此时错往往代表 “短历史不足以区分多模式/aliasing”，allocate 长历史更可能解决。
   - 行为：在更长历史表中尝试 allocate 写入 `RealEnc`，原 entry payload 不立刻改（防抖）。

3. **weak-and-wrong：倾向原地重写 payload**
   - 若 provider hit 且 `correct==false`，并且 `Conf` 已经掉到 weak（进入/处于 weak 区间）：
   - 解释：该 entry 现阶段不可信，继续“死守旧 payload”只会制造持续噪声；
   - 行为：允许 **原地 rewrite payload = RealEnc**，并将 `Conf` 重新初始化到 weak（例如 0 或 -1），`U=0`。

#### 3.3.5 Provider miss 时的分配策略

- 若 provider miss：
  - 直接在若干个更长历史表（或从最短表起）尝试 allocate 新 entry，payload 写入 `RealEnc`；
  - `Conf` 初始化为 weak；`U=0`。

#### 3.3.6 useAltOnNa 的更新（沿用经验，但以 block 粒度）

- 仅当 provider hit 且 provider 在预测时处于 weak，才更新 `useAltOnNa[startPC]`：
  - 若 alt/base 的决策更接近真实 `RealEnc`，则向 “use alt/base” 方向更新；
  - 否则向相反方向更新。

#### 3.3.7 伪代码（更新/分配）

```text
update_block(startPC, btbEntries, RealEnc, provider, alt, PredEnc):
  correct = (PredEnc == RealEnc)

  if provider.hit:
    if correct:
      provider.Conf++
      if provider_decision_differs_from_alt_or_base:
        provider.U = 1
      if is_weak(provider.Conf):  // weak-but-correct
        return  // no allocation
    else:
      provider.Conf--
      if becomes_or_is_weak(provider.Conf):  // weak-and-wrong
        provider.ExitSlotEnc = RealEnc
        provider.Conf = WEAK_INIT
        provider.U = 0
        return
      else:  // strong-but-wrong
        try_allocate_longer_tables(startPC, RealEnc)
        return
  else:
    try_allocate_tables(startPC, RealEnc)  // miss allocation
```

#### 3.3.8 参数建议（给 RTL 一个可落地的默认配置）

- **Conf 位宽**：建议先沿用现有 3-bit 饱和计数器（实现成本低，便于快速原型），并将更新从 “taken/nt 驱动” 改为 “correct/incorrect 驱动”：
  - `sat_inc`：上饱和到 `CONF_MAX`
  - `sat_dec`：下饱和到 `CONF_MIN`
- **weak 判定**：默认 `Conf in {0, -1}` 为 weak（与现有经验一致）。
- **WEAK_INIT**：allocate/rewrite 时可统一初始化为 `0`（weak），并将 `U=0`。
- **strong-but-wrong 判定**：默认可用 `Conf` 接近饱和作为 strong（例如 `Conf >= CONF_MAX-1`）。







---

下面的 Two-Taken 细节先不考虑实现，但文档保留作为后续扩展方向。

## 4. 详细设计：Two-Taken 机制 (Taken 2)

为了解决 BPU 带宽不足，我们引入轻量级 GShare 预测紧随其后的第二个 Block。

### 4.1 索引策略 (Speculative Indexing)

为了避免时序依赖，**不使用 Block 2 的 PC，而是使用 Block 1 的 PC**。

* **Index**: `Hash(PC_Block1, PHR)`
* 注意：这里假设 Block 1 Taken 后的 PHR 更新模式是固定的（或者忽略 Block 1 的 PHR 更新影响，直接用当前 PHR）。


* **Rationale**: 我们在预测 Block 1 时，顺便问一句：“在这种历史路径下，Block 1 跳完后的下一个块，大概率会跳吗？”

### 4.2 GShare 结构

* **Table Size**: 4K - 8K Entries (小容量，单读写口)。
* **Entry**: 2-bit Sat Counter (Taken / Not Taken)。
* **Output**: 仅指示 Block 2 **是否发生跳转**。

### 4.3 生成逻辑

1. **Condition**: 仅当 TAGE 预测 Block 1 为 **Taken** 时，启用 Two-Taken 逻辑。
2. **Check**: 读取辅助 GShare。
* 如果 GShare = **Not Taken**: 只发 Taken 1。
* If GShare = **Taken**: 尝试发 Taken 2。


3. **Taken 2 Target**:
* 利用 MBTB 的 **Next-Line** 能力或者 **Way 0 (First Branch)** 的信息。
* 假设 Block 2 中最早遇到的那个分支是跳转点（这是统计学上的大概率事件）。
* *注：如果 BTB 无法提供 Block 2 的 Target，则放弃 Taken 2。*




---

## 5. 讨论点 (For Discussion)

1. **MBTB Miss / 不可映射 payload 的处理**: 若 TAGE 预测的 `ExitSlotEnc` 在当前 `btbEntries` 中找不到对应的 cond entry（MBTB miss/未学到/过滤导致），推荐回退到 Base（按 MBTB 内 cond 的 `ctr` 方向预测），而不是强制 fallthrough；否则可能出现不必要的性能回退。
2. **Two-Taken 的 Target 精度**: 对于 Taken 2，我们只预测了“跳”，但默认它从第一条分支跳。对于复杂控制流（如 Taken 2 是一个 `if-else` 块），这可能不准。是否值得为 Taken 2 引入更复杂的逻辑？
3. **Loop Handling**: 这种 Exit-Slot 结构天然支持 Loop（ExitSlot 往往会稳定落在 Loop Back 的那条 cond 分支位置）。是否还需要单独的 Loop Predictor？

---

### 下一步行动计划

1. **Pattern 分析 (QEMU)**: 运行脚本，确认 SPEC06 中 `Cond -> Cond` 的比例以及 Block 2 的默认跳转倾向。
2. **原型开发**:
* 第一阶段：将 TAGE 改为 Block-Based（Exit-Slot / Cond Exit）模式，验证单 Taken 性能与资源节省情况。
* 第二阶段：加入 GShare 辅助预测器，开启 Two-Taken 发射。
