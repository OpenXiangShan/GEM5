# PC 数据流说明

## 背景
- 现仓库只面向 RISC-V，但仍沿用 gem5 通用 `PCStateBase` 抽象，导致 fetch、decoder、分支预测器、commit 等阶段都要面对多态 `PCState`，既重复更新 `_pc/_npc`，也让压缩指令（16bit）逻辑分散。
- 多数“PC 前进”操作仍默认 4B（见 `GenericISA::UPCState<4>::advance()`），而 RISC-V 的真实步长由 decoder 根据 `compressed()` 判断并写入 `PCState::npc()`，容易被后续 `advancePC()` 覆盖。
- 该文档帮助快速定位 PC 的生命周期与关键写入点，便于后续重构或调试。

## 数据流全景
1. **取指前检查**（`src/cpu/o3/fetch.cc:1889` `checkMemoryNeeds`）
   - 从 `fetchBuffer` 拷出 4B 到 decoder 的 `machInst` 缓冲，`PCStateBase` 仅参与传递当前指令地址。
2. **解码阶段**（`src/cpu/o3/fetch.cc:1975` -> `src/arch/riscv/decoder.cc:56`）
   - `Decoder::decode(pc)` 在确定指令长度后直接写 `pc.as<PCState>().npc(...)` 并设置 `compressed` 标志，这是唯一“正确”计算 `_npc` 的地方。
3. **分支预测**（`src/cpu/o3/fetch.cc:723`）
   - `lookupAndUpdateNextPC()` 在“非分支或预测不跳”时调用 `inst->staticInst->advancePC(next_pc)`，而 `RiscvStaticInst::advancePC()` 又调用 `PCState::advance()`（间接 +4）。若 decoder 刚写了 `npc = pc + 2`，此处会被覆盖。
4. **DynInst 建立 / ROB 推进**（`src/cpu/o3/fetch.cc:1998`、`src/cpu/o3/commit.cc:1373`）
   - `buildInst()` 把 `pc`/`next_pc` 封装进动态指令；commit 阶段再一次 `advancePC(*pc[tid])`，默认 4B。
5. **异常/故障路径**（例如 `src/sim/faults.cc:76`, `src/arch/riscv/faults.cc:255`）
   - Fault 处理同样使用 `advancePC()`，若没有在 fault 触发前 decode 完整 inst size，也会 fallback 到 +4。

下图（文字描述）可参考：
```
Fetch buffer bytes ──► Decoder::decode() ──► next_pc.npc = pc + {2|4}
           │                               │
           └─► DynInst.buildInst() ◄───────┘
                                      │
                       lookupAndUpdateNextPC() → advancePC()(+4)
                                      │
                               Commit / Fault
```

## 模块与接口
| 模块 | 核心函数 | 触碰 PC 的原因 | 备注 |
| --- | --- | --- | --- |
| Fetch | `processSingleInstruction()` | clone 当前 `pc`、交给 decoder 填写 `next_pc` | `std::unique_ptr<PCStateBase>` 使复制成本高 |
| Decoder | `Decoder::decode(PCStateBase &)` | 根据指令宽度写 `npc`、更新压缩标志 | 唯一 knows inst size 的模块 |
| 分支预测 | `lookupAndUpdateNextPC()` | 预测 taken 时写目标；not-taken 时 `advancePC()` | Decoupled frontend 模式还会 invalidate fetch buffer |
| StaticInst | `RiscvStaticInst::advancePC()` | 调用 `PCState::advance()`（固定 +4） | 微指令还会更新 micro PC |
| Commit | `Commit::commitHead()` 等 | 退休时 `advancePC()` 以推进 architectural PC | 假定 decode 已设置 `npc` |

## 常见陷阱
- **多处写 NPC**：decoder 与 branch predictor 都写 `next_pc`，很难看出最终谁生效。调试时建议对 `PCState::advance()`/`Decoder::decode()` 加 `DPRINTF` 或 `panic_on_overwrite`。
- **宏/微指令混用**：`curMacroop` 情况下不会重进 decoder，micro-op 的 `advancePC()` 只更新 `microPC`，实际 `npc` 仍保留上次宏指令的值，需要确认 `_compressed` 是否保持正确。
- **Fault/断点路径缺少 inst size**：异常触发时可能尚未 decode 完整指令，`advancePC()` 就会默认 +4，导致恢复后 PC 偏移。需要在 fault 前确保 `pc.compressed()` 已设定，或者在 fault handler 中读取 `StaticInst::instSize()`。

## 建议的梳理步骤
1. **自动清单**：运行 `rg "PCState" -n src`、`rg "advancePC" -n src`，并将结果分类成“读取/写入 PC”两份列表，附加用途说明。
2. **标注关键信息**：在 `src/arch/riscv/pcstate.hh` 顶部新增注释，明示“只有 decoder 负责写 npc，其他模块请使用 instSize 信息”，避免误改。
3. **封装 helper**：先实现一个 `inline void advanceByInstSize(PCState &, unsigned size)`，fetch/branch predictor/commit 全部通过它更新 `npc`，为后续完全去除 `PCStateBase` 打基础。
4. **调试辅助**：短期内可在 `PCState::advance()` 中 `panic_if(!compressedKnown)` 或输出 WARNING，提醒调用者不要直接依赖默认 +4。
5. **文档更新**：当梳理出更具体的模块交互后，把这份文件按章节补充实例（例如具体 DPRINTF 输出、常见 bug 案例），形成团队内部共识。

## 风险
- 梳理过程中若贸然修改 `advancePC()` 行为，会影响 commit、fault、checker 等多个子系统，必须在每次改动后运行至少 `scons build/RISCV/gem5.opt` + 快速仿真回归。
- 文档与代码偏离：如果文档更新不及时，可能让团队依赖过时信息。建议在提交中把相关 PR 编号/日期写入本文件顶部。

## 改进建议
1. 每当新增/修改触碰 PC 的代码路径时，要求在 MR 描述中引用本文件并说明是否影响数据流。
2. 定期（例如每个迭代）由维护者运行脚本重新生成“PC 操作列表”，对比变化，确保无人引入新的 `_npc += 4`。
3. 长期目标：在 RISC-V-only 分支中，将 `PCStateBase` 的引用逐步替换为 `RiscvISA::PCState &`，并让 `PCState::advance()` 依据 `_compressed` 调整步长，从接口层面杜绝 +4 误用。

> 本文件适合作为日常排查 PC 相关 bug 的入口，可继续扩展“案例分析”章节（例如具体 trace 片段），帮助新同学快速定位问题。若你发现新的数据流或工具，也请补充到此处。
