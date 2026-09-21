# Decode 指令融合补位优化：本地验证记录

验证执行日期：2026-09-20；报告收尾日期：2026-09-21。本文记录已执行结果，并明确尚未完成的覆盖。

## 1. 被测版本与构建

基线是昆明湖 `xs-dev` 提交
`ea8dc6b5d9c29accd6ec939cd75aaba9c5679465`，隔离工作树为
`/nfs/home/xiongye/GEM5-decode-baseline`，没有修改其被跟踪源码。
优化工作树为 `/nfs/home/xiongye/GEM5-decode-fusion`，分支
`feature/decode-fusion-compaction`；实现尚未提交，不能把基线HEAD当作优化提交SHA。

本次最后候选优化二进制的SHA-256为：

```text
ab5897e9b4ebe09c3a19b213bdf3c44ae9e50ed2036e6f6e09b74ea0fe6a11bc
```

原始基线二进制SHA-256为：

```text
c5088b4a118986939e7787ea9de964e4846badb97a64be1a95fea58c839498f3
```

原始基线和优化版均已完成带DRAMsim3的 `build/RISCV/gem5.opt` 构建。
本地已有DRAMsim3依赖以相同版本补全到两个隔离工作树，依赖提交
`29817593b3389f1337235d63cac515024ab8fd6e`；库SHA及编译环境详见
[基线身份记录](/nfs/home/xiongye/decode-fusion-validation/baseline/identity.json)。
最初缺少DRAMsim3时的checkpoint启动失败保留在历史目录，未计入任何通过结论。

构建入口：

```sh
scons build/RISCV/gem5.opt --gold-linker --ignore-style -j16 \
  M5_BUILD_CACHE=/nfs/home/xiongye/decode-fusion-validation/build-cache
```

`--ignore-style`用于构建流程；修改区域的独立风格检查和差异空白检查已另行完成。
构建日志见 [最终构建日志](/nfs/home/xiongye/decode-fusion-validation/optimized/build-dramsim.log)
和 [基线构建日志](/nfs/home/xiongye/decode-fusion-validation/baseline/build-dramsim3.log)。
可选PNG/HDF5/backtrace依赖警告不影响本轮CPU测试入口。

## 2. 实际指令定向回归

测试入口为 [run.py](../../../tests/test-progs/decode-fusion/run.py)，
执行真实RISC-V指令并调用实际Decode实现，不用另写的队列模型代替微架构。
汇编工作负载包含自检；只有成功退出才算通过，超出最大指令数不算通过。

主回归 [micro-regression-v3/report.json](/nfs/home/xiongye/decode-fusion-validation/micro-regression-v3/report.json)
的21项全部通过。随后新增并执行了关闭Predecode的两个控制流用例，见
[control-final/report.json](/nfs/home/xiongye/decode-fusion-validation/micro-regression-control-final/report.json)。
第二份报告也复跑了原有两项控制流测试；去重后共23项。
当前runner默认完整集合加 `--baseline-gem5` 和 `--tools` 包含这23项。
本轮不是同一次23项报告，证据由上述两份报告组成。

真实执行使用固定REF发布 `d30fff1ece9e-gem5-r3-multi16g-zfa-cbo` 的
`normal` 变体并开启difftest；raw微测试不使用memory-dedup。
Trace用例和启动拒绝用例按用途明确关闭difftest，不能将报告顶层
`difftest: true`解读为这些场景也做了架构逐条对比。

| 场景 | 实际结果与覆盖 |
|---|---|
| Scalar原始基线/False/True | 均成功退出，架构指令数均3383；原始基线和False的模拟ticks、操作数及原有Decode统计一致 |
| 密集融合边界 | True真实观察到一拍16条raw、8对融合；扫描与输出断言均通过 |
| Control原始基线/False/True | 均成功退出，架构指令数均1097；覆盖条件/直接分支、call/return、错误路径store自检、illegal指令精确trap及MRET |
| 关闭Predecode的Control False/True | 均成功退出，架构指令数均1097；True实际观察到Decode redirect，覆盖selfSquash及FIFO清理 |
| Vector原始基线/False/True | 均成功退出，架构指令数均1037；覆盖VSET、标量/向量交错及向量结果自检；True观察到31次跨Fetch组融合 |
| 扫描8、容量40 | 成功退出；最大每拍raw为8，实际出现扫描额度停止 |
| 扫描16、容量24 | 成功退出，覆盖最小在途预留容量；未发生覆盖队头或输出越界 |
| Trace False/True回退 | 均执行512条，模拟ticks均33633；旧路径统计一致，补位计数为零 |
| 六项启动拒绝 | loadFusion、ConstantFolding、MovImmElimination、Predecode延迟不足、容量低于在途预留、零扫描额度均按预期报错 |
| O3PipeView/PerfCCT False/True | 两组均完成现有解析/查询脚本；PerfCCT提交行数分别2361/2421 |

各True运行的日志包含真实的无效条目丢弃、后端阻塞、Commit squash和扫描额度停止。
日志解析逐拍验证下列约束，并检查相应累计计数：

```text
raw + discarded <= configured scan width
output <= 8
output == raw - fused
2 * fused <= raw
queued <= configured FIFO capacity
```

工具检查确认现有格式和入口可用，保留原始指令追踪表示；没有新增融合对象来源可视化，
不宣称两个原始指令在工具中已呈现为一条完整融合时间线。

用当前脚本重跑完整集合的命令如下；输出目录必须尚不存在：

```sh
cd /nfs/home/xiongye/GEM5-decode-fusion
python3 tests/test-progs/decode-fusion/run.py \
  --gem5 build/RISCV/gem5.opt \
  --baseline-gem5 /nfs/home/xiongye/GEM5-decode-baseline/build/RISCV/gem5.opt \
  --ref-so /nfs/home/share/gem5_ci/ref/releases/d30fff1ece9e-gem5-r3-multi16g-zfa-cbo/normal/riscv64-nemu-interpreter-so \
  --outdir /nfs/home/xiongye/decode-fusion-validation/micro-regression-rerun \
  --tools --require-16-pairs
```

微测试使用256MB内存及 `DDR3_1600_8x8`，每组对照一致；这些是正确性测试设置，
不能作为CI架构跑分配置。每个子目录的 `command.json` 保存实际argv，
例如 [Scalar True命令](/nfs/home/xiongye/decode-fusion-validation/micro-regression-v3/scalar-true/command.json)。

## 3. Checkpoint与旧路径一致性

使用SPEC06 GCC16 RVA23 no-vector的 `astar_biglakes/5863` checkpoint，
保持默认 `kmhv3.py` 与DRAMsim3，预热100k、总请求200k指令。
由于最大指令事件在提交组边界结束，最终统计段可能略多于100k；按实际统计保存，
不人为修改计数来制造True与False相同指令数。

三组均开启difftest及memory-dedup，使用上述同一固定REF发布的
`normal-dedup` 变体。具体checkpoint、REF、二进制SHA、argv及退出信息见：

- [原始基线run.json](/nfs/home/xiongye/decode-fusion-validation/baseline/astar5863-200k-dramsim3/run.json)。
- [优化False run.json](/nfs/home/xiongye/decode-fusion-validation/optimized/checkpoint-false/run.json)。
- [优化True run.json](/nfs/home/xiongye/decode-fusion-validation/optimized/checkpoint-true/run.json)。

原始基线和False两段统计分别比较8130、7401个共同的模拟统计项，**没有差异**，
没有删除原有统计；仅新增补位相关统计。明确排除host速率、耗时和内存等非架构统计。
证据见 [baseline-false-comparison.json](/nfs/home/xiongye/decode-fusion-validation/optimized/baseline-false-comparison.json)。

True也完成相同checkpoint短程difftest；测量段观察到246次跨Fetch组融合、
最大FIFO占用40且无占用溢出。此结果验证的是短程功能与实际启用路径，
**不产生SPEC分数，也不据约100k指令的测量段推断性能提升或退化**。

可复现入口如下，输出目录应使用新路径；原始基线运行不传新增优化参数：

```sh
python3 /nfs/home/xiongye/decode-fusion-validation/run_checkpoint_smoke.py \
  --repo /nfs/home/xiongye/GEM5-decode-fusion \
  --output /nfs/home/xiongye/decode-fusion-validation/checkpoint-false-rerun \
  --compaction False
python3 /nfs/home/xiongye/decode-fusion-validation/run_checkpoint_smoke.py \
  --repo /nfs/home/xiongye/GEM5-decode-fusion \
  --output /nfs/home/xiongye/decode-fusion-validation/checkpoint-true-rerun \
  --compaction True
python3 /nfs/home/xiongye/decode-fusion-validation/compare_checkpoint_stats.py \
  /nfs/home/xiongye/decode-fusion-validation/baseline/astar5863-200k-dramsim3 \
  /nfs/home/xiongye/decode-fusion-validation/checkpoint-false-rerun \
  --output /nfs/home/xiongye/decode-fusion-validation/checkpoint-comparison-rerun.json
```

## 4. SMT回退与生命周期覆盖

SMT False/True两次运行均正常退出，架构指令数6763、模拟ticks2176488、操作数4719。
比较6714个共同的模拟统计项，没有差异；True回退旧路径，未执行补位扫描。
证据见 [SMT统计对比](/nfs/home/xiongye/decode-fusion-validation/smt-comparison.json)
及 [SMT True命令](/nfs/home/xiongye/decode-fusion-validation/smt-true/command.json)。

该SMT验证使用两线程raw自检工作负载、512MB及DDR3模型，显式
`--disable-difftest`。它证明本场景回退路径及输出保持一致，不能宣称已通过SMT多核参考模型difftest。

**主动Drain/resume没有通过，已确认干净基线也复现，不能归为新路径独有回归。**
测试包装器在固定模拟tick暂停后调用 `m5.drain()`；进入前恢复原始 `m5.simulate`，
避免递归包装。原始基线和True各10秒超时，False先前120秒超时，均未返回drain完成，
也尚未进入resume。该基线的 [Fetch::drainStall](../../../src/cpu/o3/fetch.cc)
为空，Commit依赖的停止Fetch流程不完整；本轮没有扩大到Fetch修复。

证据与完整命令见 [SMT/drain记录](/nfs/home/xiongye/decode-fusion-validation/smt-and-drain-review.md)
和 [汇总JSON](/nfs/home/xiongye/decode-fusion-validation/smt-and-drain-summary.json)。
False的大型调试日志已无损压缩保存。该项明确是未完成的动态验收；生命周期接口虽已
接入新FIFO，线程退出、CPU切换/takeover及所有非空FIFO恢复状态仍不能仅凭源码检查
或成功程序退出认定通过。

### 融合异常重执行补测（不计入difftest通过）

额外使用保留编码 `C.LUI t0,0` 紧接 `ADDI t0,t0,1`，使融合体执行时返回异常。
三组均实际记录到一次 `Fault on fusion instruction, re-execute without fusion`，
并在关闭difftest的自检运行中正确拆开重跑、检查 `mcause=2`、原始2字节指令的 `mepc`、
恰好一次trap以及最终寄存器结果。原始基线、False、True都正常退出，均为25条架构指令、
25个操作、233100模拟ticks。

该用例的固定NEMU参考模型对保留C.LUI编码处理不同；开启difftest时三组均在同一场景
产生参考模型差异。因此这只能证明Gem5当前异常模型内的融合重执行自检通过，
不能声称该保留编码通过difftest。没有为此修改ISA、参考模型或默认回归集合。

证据见 [融合异常自检报告](/nfs/home/xiongye/decode-fusion-validation/micro-fusion-fault-selfcheck/report.json)，
原始命令及日志保存在同目录；可复现汇编见
[fusion-fault.S](../../../tests/test-progs/decode-fusion/fusion-fault.S)。

## 5. 已知覆盖边界与后续CI

本轮验证已覆盖实际融合、16raw/8对边界、跨Fetch组、Predecode开关下控制恢复、
Trace/SMT回退、工具解析和一个真实checkpoint的False一致性/True difftest。
以下仍属于未穷尽或未完成的范围：

- 任意合成FIFO状态：特定配对间恰好一个无效条目、精确8/9或16/17位置的所有组合，
  跨FTQ拒绝及不同loopIteration等每个合法性分支尚未逐项注入验证。
- 任意同时到达的Commit/selfSquash顺序、版本回绕、队列内较老幸存项及迟到输入的
  所有边界组合，尚未由完整内部状态单测穷尽。
- 融合异常禁融重放与精确Fault边界的全部类型，不由普通illegal指令测试替代；
  loadFusion、ConstantFolding和MovImmElimination仍是不支持组合。
- 普通load值预测、完整向量异常、长时间负载、线程清理和CPU takeover的独立矩阵未完成。
- 主动Drain/resume在原始基线、False和True均超时，需单独处理已有Fetch停止流程后再验收；不得把成功程序退出等同于显式drain通过。

本轮没有触发145个checkpoint的完整性能CI，也没有可发布的总分或29个子项分数。
后续仍按固定xs-dev提交A原始baseline、同一优化提交B的False及True三组执行：
`baseline-for-decodeinst`、`fause-for-decodeinst`、`true-for-decodeinst`。
先以A/False验证旧路径，再由False/True比较性能，具体范围见
[实施计划](decode-fusion-compaction-plan.md)。
