# O3CPU Trace-Driven Simulation

XiangShan O3CPU runs that reuse the full pipeline while sourcing instructions from external traces.

## 概览

- 支持 ChampSim（`.bin/.gz/.xz`）、CBP2025（`.gz`）与 TRACERTL/NEMU（`.trace.zstd`/raw，48 B 记录）格式，通过 `TraceReader` 统一接口实现。
- Fetch 拥有 trace reader，维护 1024 条缓冲 + 4096 条历史窗口，可在不触碰文件指针的情况下软回滚；每隔 64 个 seqNum 生成一次检查点。
- `TraceStream` 压缩通道：raw / gzip（fork+pipe）/ xz（fork+pipe）/ **zstd（libzstd 进程内流式解压，按 magic 探测）**。
- 默认地址映射（`BaseO3CPU` 参数）：`traceAddrMapMode=linear`，`traceAddrBase=0x80000000`，`traceAddrSize=0x40000000`，`traceAddrPageAlign=true`，可改为 `hash`。
- 关闭 trace mode 时对正常 FS 行为无影响；trace 模式下自动关闭 difftest。

## 运行示例

```bash
scons -j$(nproc) --gold-linker build/RISCV/gem5.opt

./build/RISCV/gem5.opt configs/example/kmhv3.py \
  --enable-trace-mode \
  --trace-file=/path/to/trace.champsimtrace.xz \
  --trace-format=champsim \
  --warmup-insts-no-switch=50000000 \
  --maxinsts=1000000 \
  --trace-enable-decoupled-bp \
  --trace-mispredict-penalty=8 \
  --stats-file=m5out/stats.txt
```

`kmhv3.py` 在启用 trace 模式时放宽 `--generic-rv-cpt` 的必选性，并打印 trace 文件、格式与 `--maxinsts` 摘要。

### Trace 专用 CLI（`configs/example/kmhv3.py`）

- `--trace-format {champsim,cbp2025,tracertl}`（默认 `champsim`；`nemu` 为 `tracertl` 别名）
- `--trace-use-synthetic-enc`：强制合成编码，忽略 trace 自带的真实编码（tracertl A/B 诊断；默认用真实编码）
- `--warmup-insts-no-switch=N`：仅重置统计的 warmup（不切 CPU）
- `--trace-enable-decoupled-bp`：在 trace 模式下走解耦前端
- `--trace-checkpoint-interval=N`（默认 64）
- `--trace-disable-bp-validation`：跳过 BP 与 trace 的一致性校验
- `--trace-mispredict-penalty=N`：mispredict 后 Fetch stall 周期（默认 8）
- `--trace-disable-wrongpath` / `--trace-wrongpath-use-traceinst`：控制 wrong-path 注入

地址映射参数（CPU 级别）：`traceAddrMapMode`、`traceAddrBase`、`traceAddrSize`、`traceAddrPageAlign`。

## 格式细节

- **ChampSim**：PC、branch flag/结果、2 dst + 4 src 寄存器、2 dst + 4 src 地址。`TraceStream` 通过 `gzip -dcq` / `xz -dc` 处理压缩输入。
- **CBP2025**：包含 nextPC、分支类型/结果、有效地址与 size、寄存器依赖与值；gzip 支持。
- **TRACERTL/NEMU（`tracertl`，别名 `nemu`）**：48 B/条小端记录，无文件头，zstd 压缩或裸文件（按 magic `28 B5 2F FD` 探测，不看后缀）。字段：`pc_va`（u64，4 对齐）、`pc_pa`（恒 0，不可用）、`mem_va`（u64，memory_type≠0 时有效）、`mem_pa`（脏数据，忽略）、`target`（u64，分支/强制跳转目标）、`instr`（u32，**真实 RV64 编码**）、`mem_type_size`（低 4 位 type：0/1/2=无/load/store；高 4 位 size code：0..3=1/2/4/8 B）、`branch_type`（0..7）、`branch_taken`、`exception`（≠0 为强制跳转，目标在 `target`）。

### TRACERTL 关键语义（实测自 gwt-v2-nemu-format-simpoint-interval 语料）

- **真实编码直喂 decoder**：reader 通过 `TraceInstruction::setInstBits()` 发布 `instr` 字段，`createMachInstFromTrace` 优先使用真实编码，保留指令 mix、功能单元与延迟（与 RTL 侧 TraceRTL 一致）。`--trace-use-synthetic-enc` 强制退回合成编码（A/B 诊断）。**注意**：此格式下 `traceTrainBranches=false` 仅使分支记录退回 NOP 合成编码（保留该 flag"不喂真实分支 opcode"的文档语义），非分支记录仍用真实编码。
- **分支 imm 全为 0**（B 型与 J 型均如此）：所有控制流目标只存在于 `target` 字段，由 IEW/decode 的 trace nextPC 覆盖（`setTraceBranchInfo`）驱动。
- **`exception != 0` 且非分支**：走 commit 侧 `TraceCtrlFlowFault` 强制重定向（与 CBP2025 trap 通道相同）；其编码若为控制流 opcode（beq/jal/jalr）会被消毒为真实 NOP `0x00000013`，以保持 commit difftest 的 InstType 一致且不进合成路径。
- **同 PC 双编码**：`pc_discontinuity` 占位（`0x00000033` + exception=0x01）与后续真实编码并存；reader 记录 `traceReader.stats.mixedEncodingPc` 计数。
- RVC（16 位编码）v1 不支持：告警并按 4 字节合成编码回退。

## 行为与限制

- Branch target 以“上一条 pending + 下一条 PC”推断（缺失 target 的 trace 会用下一条 PC 修正），必要时可修补 taken target mismatch。
- ChampSim 访存值是按地址生成的确定性伪值；未写入 mem size（`memSizes` 为空），需要尺寸时请自行扩展。
- Reader 统计挂在 CPU 名下（通过 Fetch 构造时传入 parent stats）。
- 历史窗口足够覆盖短期回滚；超过窗口时会降级为 `seekToInstruction` 重读 trace。

## 调试

常用调试标志：`--debug-flags=Fetch,TraceReader,O3CPU`（检查前端 + reader），BP 相关可加 `BPred,DecoupleBP`。统计关注 `system.cpu.fetch.*`、`system.cpu.branchPred.*`、`system.cpu.ipc`。离线查看 trace 片段可用 `util/trace/dump_champsim_trace.py --start-index/--count`。
