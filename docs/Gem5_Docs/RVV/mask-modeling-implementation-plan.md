# MaskRAT 及相关专利 · GEM5 建模实施方案

> 目标：在 `~/GEM5`（XiangShan Kunminghu v3 模型）上实现 **MaskRAT**、**向量掩码细粒度存储与读取**、**向量掩码多对一唤醒**，以及配套的 **move 消除 / 零长度 NOP**，使模型能够反映这些硬件优化的**时序与性能**。
>
> 原则：**以性能/时序建模为主，实现细节不必逐位对齐 RTL**；功能正确性由 difftest 兜底。每个里程碑单独可编译、可回归、可量化。
>
> 相关文档：`docs/Gem5_Docs/RVV/mask-modeling-plan.md`（已有 P1/P2/P3 方案）、`mask-hardware-diagram.md`、服务器专利交底书。

---

## 0. 目标与范围

**做：**
- 让 mask 生产者/消费者在模型里体现"按行重命名、按行写回、按所需行唤醒"的时序。
- 体现 MRF 行池（容量/分配失败）、读适配器延迟、读/写端口占用。
- 提供参数开关与统计，支持与现有模型 A/B 对比。

**不做（本期）：**
- 不改 ISA 语义结果（difftest 必须继续通过）。
- 不重构前端/预测器/访存链路。
- 不追求 ptag 位宽、位图选择器流水线等 RTL 细节的逐位一致。

**Baseline 说明：** 现有模型已用 `VecTempReg0+i` 让生产者 μop 并行，**已经比"整寄存器 WAW 串行"的 RTL 基线更接近 MaskRAT**。因此本方案的对比基准是"改动前的 GEM5 模型"，主要增量是：**去掉 merge μop + 按行唤醒 + MRF 资源/端口**。若需要模拟"无 MaskRAT 的 RTL 基线"，另加 `maskWholeRegWAW` 开关强制串行（可选）。

---

## 1. 现状与已有基础

### 1.1 已完成（M1 骨架，分支 `maskRAT-0922` / `MaskRAT-duxy`）
- 新增寄存器类 `MaskRegClass`（32 架构寄存器）。
- `PhysRegFile` 增加 `maskRegFile` 与 `maskRegIds`，纳入 `UnifiedFreeList`。
- `BaseO3CPU.py` 增加 `numPhysMaskRegs / mrfEntries / mrfRowsPerEntry / mrfRowBits / mrfReadPorts / mrfWritePorts`。
- `cpu.cc` 增加 `maskRegfileReads/Writes` 统计，并接通 getReg/setReg。
- ISA/reg_class/simple_thread/dyn_inst/inst_queue 已接通。

### 1.2 当前 mask 功能模型（未改）
- **生产**：mask 生产者按 LMUL 拆 iLMUL 条 μop，各写 `VecTempReg0+i`（`vector_arith.isa`：`dest_reg_id = VecTempReg0 + _microIdx`），最后一条 `VMaskMergeMicroInst`（`insts/vector.hh:571`）合并进 vd。
- **消费**：`SET_VM_SRC()`（`vector_arith.temp.isa:22`）把 v0 作为 `RegId(VecRegClass,0)` 整读，`VM_REQUIRED()` 取 `tmp_v0.as<uint8_t>()`。
- **唤醒**：`scoreboard` 扁平 bool；`IssueQue::wakeUpDependents` 走 `subDepGraph[dst->flatIndex()]` 逐源 `markSrcRegReady(srcIdx)`（一对一）。

### 1.3 关键代码位置索引
| 关注点 | 位置 |
|---|---|
| mask 生产 ISA 生成 | `src/arch/riscv/isa/vector/base/vector_arith.isa:500-560` |
| mask 生产/消费模板 | `src/arch/riscv/isa/vector/base/vector_arith.temp.isa`（`SET_VM_SRC`/`VM_REQUIRED`/`VectorIntMaskMacroConstructor`） |
| mask 合并 μop | `src/arch/riscv/insts/vector.hh:571` (`VMaskMergeMicroInst`) |
| 寄存器类 | `src/cpu/reg_class.hh`、`src/arch/riscv/isa.cc:339` |
| 物理寄存器堆 | `src/cpu/o3/regfile.{hh,cc}` |
| 重命名源/目的 | `src/cpu/o3/rename.cc` `renameSrcRegs`(1109) / `renameDestRegs`(1177) |
| 重命名表 | `src/cpu/o3/rename_map.{hh,cc}` |
| 空闲表 | `src/cpu/o3/free_list.{hh,cc}` |
| 记分牌 | `src/cpu/o3/scoreboard.hh` |
| 发射队列/唤醒 | `src/cpu/o3/issue_queue.{hh,cc}`（`insert` ~1240、`wakeUpDependents` 915） |
| DynInst 源/目的数组 | `src/cpu/o3/dyn_inst.hh`（`_srcIdx`/`_readySrcIdx`/`Arrays`） |
| 参数 | `src/cpu/o3/BaseO3CPU.py`、`configs/example/kmhv3.py` |

---

## 2. 总体设计决策

| 编号 | 决策 | 理由 |
|---|---|---|
| **D1** | **mask 数据落到 `MaskRegClass` 物理行**：1 个物理寄存器 = 1 个 MRF row（容器 VLENB，仅本行 slice 有效）。生产者 μop `k` 写 `base+k`，**删除 `VMaskMergeMicroInst`**。消费者读 N 行并 OR 出 mask。 | 只有行级物理寄存器才能让"按行唤醒"成立；顺带去掉 merge 串行点。 |
| **D2** | **ptag 组重命名**：firstUop 从 `MaskFreeList` 分配 `iLMUL` 个连续且 `ptag%iLMUL==0` 的行；其余 μop dest = `base+microIdx`，不分配。 | 对齐 RTL 的"一指令一 ptag"；`base|k == base+k`。 |
| **D3** | **多对一唤醒**：IQ 中每个 mask 源操作数保存 `{base,N,C 计数器}`；行写回按高位范围匹配累加 C，`C==N` 才置 ready。 | 对齐专利；一个 mask 源对应 N 个行写回。 |
| **D4** | **读适配**：功能上消费者读 `[base+start, +N)` 行并 OR；时序上按 单行/多行采样/部分行 三模式叠加可配置延迟与端口占用。 | 功能正确 + 时序可标定。 |
| **D5** | **参数开关 + 统计**；baseline 用改动前 commit/独立 binary 对比。 | 可量化、可回退。 |

> **简化取舍（明确放弃 RTL 逐位对齐）**：不做 16bit 字节展开的实际存储（GEM5 功能 mask 是"1 元素 1 bit"的 packed 格式）；不做 ptag 位图选择器流水线；不做 MRF 物理面积。这些对"性能增量"影响小，或只作为统计/延迟参数体现。

---

## 3. 数据结构设计

### 3.1 MaskFreeList（新的空闲行表，位图）
`src/cpu/o3/free_list.hh/.cc` 新增独立类（不改 `SimpleFreeList`）：

```cpp
class MaskFreeList {
    std::vector<bool> freeBitmap;      // 大小 numPhysMaskRegs (=328)
    unsigned reserveRows = 8;          // ptag 0..7 = MRF entry0 只读全1，永不分配
  public:
    void init(unsigned numRows, unsigned reserve);
    // 分配 iLMUL 个连续对齐行，返回 base；失败返回 INVALID
    std::optional<unsigned> allocate(unsigned iLMUL);
    void free(unsigned base, unsigned iLMUL);
    bool canAllocate(unsigned iLMUL) const;
    unsigned numFree() const;
};
```
- 对齐分配：从 `reserveRows` 起按 `iLMUL` 步长扫描 `freeBitmap[start..start+iLMUL-1]` 全 1。
- 快照/恢复：随 `SnapshotGenerator`（先做整表复制；若面积敏感再压缩）。

### 3.2 MaskRAT（32 条目的 spec/arch 元数据）
`src/cpu/o3/rename.hh`（或新文件 `mask_rat.hh`）：

```cpp
struct MaskEntry {
    uint16_t ptag = 0;   // 指向 base 行
    uint8_t  iLMUL = 1;
    uint8_t  dSEW  = 8;
    bool     valid = true;
};
class MaskRAT {
    std::array<MaskEntry, 32> spec;
    std::array<MaskEntry, 32> arch;   // ptag=0,iLMUL=1,dSEW=8 初始化
  public:
    MaskEntry lookupSpec(unsigned v) const;
    void setSpec(unsigned v, MaskEntry e);
    void commit(unsigned v, MaskEntry e);   // 更新 arch
    void restoreFromArch();                 // squash 无快照时兜底
};
```
- **恢复**：优先 `SnapshotGenerator`（branch rename 时保存 spec+arch），squash 时恢复并 `restoreFromArch`。若实现成本高，先接受"快照点恢复"的近似精度（性能建模可接受）。

### 3.3 DynInst 新增字段
`src/cpu/o3/dyn_inst.hh`：
- 生产者：`bool maskProducer`、`uint8_t maskMicroIdx`、`uint8_t maskILMUL`、`uint8_t maskDSEW`、`bool maskFirstUop`。
- 消费者：`bool maskConsumer`、mask 源在 `srcIdx` 中的下标 `int maskSrcIdx`、`uint8_t maskN`、`uint16_t maskBase`。
- 行阻塞状态（用于 IQ counter）：`uint8_t maskWaitC`、`uint8_t maskWaitN`、`uint16_t maskWaitBase`、`bool maskWaitActive`。

### 3.4 IssueQue 新增结构
`src/cpu/o3/issue_queue.hh`：
```cpp
// base 行 -> 等待中的消费者列表（number 很小，线性搜索即可）
std::vector<std::vector<std::pair<uint16_t /*base*/, DynInstPtr>>> maskRangeWaiters;
```
（或按 base 索引的 `unordered_map`。）

---

## 4. 分阶段实施

> 每个里程碑结束都必须：`scons build/RISCV/gem5.opt --gold-linker -j64` 通过 + 冒烟 difftest 通过。

### M0 · 开关与统计骨架（0.5–1 天）
**目标**：建立可灰度、可观测的框架，默认不改变行为。
- `BaseO3CPU.py` 增加 `Param.Bool`：
  - `enableMaskModel`（默认 false）
  - `enableMaskMultiToOneWakeup`（默认 true，仅在 enableMaskModel 下生效）
  - `maskReadAdapterLatency`（默认 1）
  - `enableMaskMoveElim`、`enableMaskVlZeroNop`（默认 false）
- 在 `kmhv3.py` 显式打开 `enableMaskModel=True`。
- 在 `Rename`/`IssueQue`/`CPU` 增加统计组（见 §5）。
- **验收**：两种配置都编译；difftest 通过且统计为 0/无异常。

### M1 · 行池与 MaskRAT 元数据（骨架收尾，1–2 天）
**目标**：能分配/释放 ptag、维护 32 条目元数据，但**功能仍走旧路径**。
- 实现 `MaskFreeList`、`MaskRAT`；在 `CPU`/`Rename` 中实例化并 `init`。
- `renameDestRegs` 增加分支（仅登记元数据，不改 `renamedDestIdx`）：mask 生产者 firstUop 分配 ptag 并 `MaskRAT.setSpec`，其余 μop 跳过。
- commit 处：mask 生产者更新 `arch` 并释放旧 ptag。
- squash：调用快照恢复。
- **验收**：difftest 不变；`maskPtagAlloc/Fail` 统计随 RVV 负载非零；MRF 占用合理。

### M2 · 生产者按行写 + 删除 merge（功能重构，3–5 天）
**目标**：mask 生产者 μop 直接写行，消费者读行并 OR；**这是功能正确性风险最高的一步**。
- ISA 侧（`vector_arith.isa` / `vector_arith.temp.isa` / `insts/vector.hh`）：
  - mask 生产者微指令目的改为 `RegId(MaskRegClass, vd)`，暴露 `microIdx`、`iLMUL`、`dSEW`、`isFirstMicroop`。
  - **删除 `VMaskMergeMicroInst`** 的插入（`VectorInt/FloatMaskMacroConstructor`）。
  - 生产者 execute 保持"在全局 bit 偏移写自己那一行 slice"（复用现有 `offset = VLEN/sew*microIdx`），只是目的寄存器换成行物理寄存器。
- Rename：
  - `renameDestRegs` mask 分支：firstUop `base=MaskFreeList.allocate(iLMUL)`，`renamedDestIdx = PhysRegId(MaskRegClass, base)`；非 firstUop `renamedDestIdx = base+microIdx`，并把 `base` 存在 DynInst（`maskBase`）供后续 μop 使用（同一 macro 的 μop 共享）。
  - `renameSrcRegs` mask 分支：`base=MaskRAT.lookupSpec(v0).ptag`，写 `renamedSrcIdx = base`，记录 `maskBase`。
- 消费 execute（`VM_REQUIRED`）：
  - 新增 helper `readMaskRows(xc, base, N, out)`：读 `base..base+N-1` 行物理寄存器并 **OR** 到输出 buffer；替换原整读 `RegId(VecRegClass,0)`。
  - tail（`vl..VLEN`）位按 agnostic 填 1。
- **验收**：RVV 定向单测 + difftest 通过；生产者 μop 数 == iLMUL；`maskRegfileReads/Writes` 合理。

### M3 · 按行读适配与消费 row 计算（2–3 天）
**目标**：跨 SEW 正确读取，并体现读适配模式。
- 复用 `plan.md` 公式：`maskPerRow = VLEN/dSEW`，`N = ceil(dSEW/srcSEW)`，`start = uopIdx*N`；A 类 `N=iLMUL`。
- 译码信号（`src1UseMask/src2UseMask/useAllMask/v0MaskNum`）：先在 `isa/vector/base/*.isa` 给消费微指令打标记；`v0MaskNum` 的 narrow 情形先按 1 处理，后续补齐。
- 消费超范围：`uopIdx*N*maskPerRow > maskPerRow*iLMUL` → 该 uop 置 `vm=1`、不读 MRF（对齐专利 tail 填 1）。
- 读适配延迟（D4）：单行 `1` 拍；多行采样/部分行按 `maskReadAdapterLatency` 附加到 execute 延迟。
- **验收**：定向微基准（跨 SEW 链）统计 N/模式正确；difftest 通过。

### M4 · 多对一唤醒（2–3 天）
**目标**：N 个行写回汇聚为一个 mask 源就绪。
- `IssueQue::insert`：
  - 若 `inst->maskConsumer` 且 `N>1`：`C0 = count(scoreboard[base..base+N-1] ready)`；写入 `maskWait{C,N,base,Active}`；把 `{base, inst}` 加入 `maskRangeWaiters[base]`；`maskSrcIdx` 暂不置 ready，除非 `C0==N`。
- `IssueQue::wakeUpDependents`：
  - 对每个 dest 若为 `MaskRegClass`：查 `maskRangeWaiters` 命中 `[base,base+N)` 的等待者（**高位匹配**：忽略低 `log2(N)` 位），`C++`；`C==N` → `markSrcRegReady(maskSrcIdx)` 并移出等待列表。
  - 同周期多命中：`C += H`；入队边界用 scoreboard 初值，避免漏计/重计。
- `enableMaskMultiToOneWakeup=false` 时退化为"等全部 iLMUL 行"（近似整寄存器唤醒），用于 A/B。
- **验收**：专利实例序列（`vmseq e64 m8` → `vle16 v0.t`）中 C0/C1 分别在 4 行凑齐后唤醒；统计 `maskRangeWakeups`、平均等待行数、`maskSrcWaitCycles`。

### M5 · MRF 端口/容量时序细化（可选，2–3 天）
- 读端口：`mrfReadPorts` 忙计数，超限则该周期不可读（退化为下拍）。
- 写端口：`mrfWritePorts` 同理会造成行写回排队。
- 容量：`MaskFreeList.canAllocate` 失败 → rename 停摆（统计 `maskPtagFail`、`maskRenameStallCycles`）。
- **验收**：定向微基准对齐 RTL 参数的定性趋势（条目/端口越小越易 stall）。

### M6 · 配套消除（可选，各 1–2 天）
- **move 消除**（`enableMaskMoveElim`）：
  - `vmmv.m vd,vs` / `vmvNr.v`：`MaskRAT.spec[vd] = MaskRAT.spec[vs]`，不分配、不写 MRF。
  - `vmset.m vd` / `vmv.v.i vd,-1`：`MaskRAT.spec[vd] = {ptag=0, iLMUL, dSEW}`。
- **零长度 NOP**（`enableMaskVlZeroNop`）：decode 识别 `vl==0` + NOP 资格 → rename 关闭目的分配、仅进 ROB；ROB 提交时释放。参考专利《零长度处理》。
- **验收**：相应指令 rename 不再分配 ptag/向量目的寄存器；difftest 通过。

---

## 5. 参数与统计

### 参数（`src/cpu/o3/BaseO3CPU.py`）
| 参数 | 默认 | 说明 |
|---|---|---|
| `enableMaskModel` | false | MaskRAT 建模总开关 |
| `enableMaskMultiToOneWakeup` | true | 多对一唤醒；false 时等全部行 |
| `maskReadAdapterLatency` | 1 | 读适配附加周期 |
| `enableMaskMoveElim` | false | move 消除 |
| `enableMaskVlZeroNop` | false | 零长度 NOP |
| `numPhysMaskRegs` | 328 | 已加（MRF 行池） |
| `mrfEntries/mrfRowsPerEntry/mrfRowBits/mrfReadPorts/mrfWritePorts` | 41/8/16/8/8 | 已加 |

### 统计（`Rename`/`IssueQue`/`CPU` 各加 `statistics::Group`）
- `maskPtagAlloc` / `maskPtagFree` / `maskPtagFail` / `maskOccupancy`
- `maskProducerUops` / `maskConsumerUops`
- `maskRegfileReads` / `maskRegfileWrites`（已加）
- `maskRowsRead`（按单行/多行/部分行分类）
- `maskReadAdapterCycles`
- `maskRangeWakeups` / `maskSrcWaitCycles` / `maskPartialWakeups`
- `maskMoveElim` / `maskVlZeroNop`

---

## 6. 验证

- **功能**：`kmhv3.py --generic-rv-cpt` 跑 SPEC/GemsFDTD 冒烟 + RVV 定向测试，**difftest 必须通过**。
- **单测**：参照 `src/arch/riscv/vector_decode_pipeline.test.cc`；新增 mask row 映射与唤醒的 C++ 单测。
- **性能**：复用 `regression_rvv_rename_bandwidth/`、`regression_vector_padding_*`；对比 `enableMaskModel` 开关前后的 stats。
- **A/B**：同一 checkpoint，`enableMaskModel` on/off 各跑一遍，比较 IPC、VecMaskStall 类指标、`maskSrcWaitCycles`。
- **构建**：`scons build/RISCV/gem5.opt --gold-linker -j64`；单测 `scons build/RISCV/unittests.opt -j100 --unit-test`。

---

## 7. 风险与回退

| 风险 | 影响 | 缓解 |
|---|---|---|
| M2 功能重构破坏 difftest | 高 | 分两步：先只改目的为 MaskRegClass 行、保留 merge 写 vd；验证后再删 merge |
| 动态行数 N 与源操作数/数组尺寸冲突 | 中 | 采用 **D3 IQ 计数器**（保持 1 个 mask 源），避免扩宽 `srcRegIdxArr` |
| MaskRAT spec 恢复不精确 | 中 | 优先 SnapshotGenerator；退化用 archTable 兜底，仅影响极端 squash 的时序精度 |
| 与 VMST/mv.m2v、移动消除交叉 | 中 | 本期不动 VMST；`vmmv.m` 等只做 MaskRAT 侧消除，向量侧 move 另行处理 |
| 现有 `mask-modeling-plan.md` 分工重叠 | 低 | 本方案为其 M1 之后的执行细化，冲突处以本方案为准并同步回 plan |

**回退**：所有改动受 `enableMaskModel` 保护；出问题设 false 即回到当前模型。

---

## 8. 建议落地顺序（最短见效路径）

```
M0 开关统计 ──▶ M1 行池+MaskRAT元数据 ──▶ M2 按行写/删merge ──▶ M4 多对一唤醒
                                              │
                                              ├──▶ M3 跨SEW读适配（可与M4并行）
                                              └──▶ M5 端口/容量（可选）
                                                        │
                                              M6 move消除 / 零长度NOP（可选）
```

- 若只要"能看出 MaskRAT 效果"：先做 **M0+M1+M2+M4**（去掉 merge + 按行唤醒），即可量化主要增量。
- 若要"对齐 RTL 参数趋势"：补 **M3+M5**。
- 若要"完整覆盖专利"：补 **M6**（与已有 `mask-modeling-plan.md` 的 P2 一致）。
