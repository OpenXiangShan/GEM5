# BPU 预测数据模型重构记录

## 目标与约束

从 `75569f588b` 开始，将分支描述、BTB 存储和阶段预测分开，删除重复字段及转换。
保留 `numDelay`、组件优先级、气泡消耗顺序、训练时点、历史更新和 FTQ 行为。
当前任务不重做真实逐级流水，也不改变 PairTAGE 的算法或 two-taken 策略。

## 分阶段执行

1. TAGE 显式使用预测时的基础方向，删除对 BTB counter 的读取及训练时 bool/counter 转换。
2. 分离 BTB 存储表项和预测候选；counter/tag 留在存储侧，方向预测输出明确有效性。
3. 阶段结果使用统一候选数组，消除独立方向/间接目标/RAS 目标映射；组件接口按职责收窄，顶层负责各级组合。
4. 统一公共分支属性与描述，迁移真实分支结果及 PairTAGE，删除重复表示与适配。

各阶段可保留短期适配，最终删除已迁移的旧表示。预测时基础方向与原始目标等训练信息应在 metadata 中显式保存。

## 验证

- 首先从 base 构建独立 baseline binary，保存测试配置、命令、退出原因和完整 stats。
- CoreMark 固定指令窗口；frontendtest 选择条件分支、call/return、动态间接跳转。
- 每阶段构建并运行同一测试集，比较 committed instructions、numCycles、IPC、分支/覆盖/历史相关统计；host 时间和吞吐排除。
- 相关预测器单元测试覆盖 fallback、方向、目标、历史、训练和恢复。
- 最终独立正确性审查；运行 style 检查。
- 最终对 base/head 用相同配置和 SPEC06 0.3c profile 触发 CI，核对实际被测 SHA、coverage、切片和分数。

## 证据记录

本地过程输出存放 `/tmp/bpu-data-refactor/`。阶段结果与 CI run 在完成后补充。

### 阶段 1 / 2

- 阶段 1：TAGE 内部 lookup 显式接收 base 方向；训练读取预测 metadata，删除 bool→counter→bool 转换。30 项 TAGE 测试通过。
- 阶段 2：`BranchPrediction` 分离 BTB 存储。counter/tag 不进入阶段候选；base 方向、MicroTAGE 所需的 base 强度和来源显式保存。93 项相关单元测试通过。
- 两阶段均运行 8 组 baseline 对照（CoreMark 300k 窗口，alternating/return/indirect 完整运行；PairTAGE 开/关），所有非 host 模拟统计逐项一致。

## 新会话交接（用户要求暂停，2026-11-03）

### 当前版本与完成边界

- 工作目录：`/nfs/home/yanyue/workspace/GEM5_review`。
- 当前分支：`refactor/bpu-prediction-data`；base：`75569f588b`。
- 第一阶段提交：`533f3b8ef9 cpu: Pass base direction explicitly to TAGE training`。
- 第二阶段提交：`849e2714bf cpu: Separate branch candidates from BTB storage entries`。
- 上述提交仅在本地，尚未 push、创建 PR 或触发 CI。
- 暂停前源码工作区干净；本交接段为随后新增、尚未提交的文档修改。
- 第 1 阶段已完成内部接口清理；第 2 阶段只完成存储/候选分离。高级预测有效性、顶层合成尚未实现，不能宣称四阶段完成。

### 已改内容

1. `BTBTAGE::generateSinglePrediction()` 接收 `BranchInfo` 与显式 `baseTaken`。训练直接取预测 metadata 的 `basePred`，不再构造带伪 counter 的 `BTBEntry`。exact-history upper bound 同步改造。
2. `common.hh` 新增 `BranchPrediction : BranchInfo`，保存 `valid/taken/baseTaken/baseStrong/source`。`FullBTBPrediction::btbEntries` 改为此类型；BTB 存储仍用 `BTBEntry`，保留 counter/tag。
3. MicroTAGE、ITTAGE、MGSC、PairTAGE 及 two-taken teacher 的候选接口同步迁移。UBTB/ABTB 在重新写存储时显式转回 `BranchInfo` / `BTBEntry`。
4. `baseStrong` 保留原 MicroTAGE `abs(2 * ctr + 1) == 5` 的条件，不能自行替换为泛化的饱和判断。resolved 训练旧实现把 base 编码为 0/-1，因此该路径显式 `baseStrong=false` 保持原行为。

### 下一阶段必须完成的工作

- 当前 `BranchPrediction.taken` 尚非阶段方向的唯一来源；`condTakens/indirectTargets/returnTarget` 仍存在，迁移时统一为候选自身的方向与预测目标，并删除旧映射、查找和转换。
- TAGE 当前 wrapper 仍从候选读取基础方向并内部完成 fallback。最终应返回高级预测的有效性及方向，由顶层保留 base/TAGE/SC 的选择策略；预测时 base 信息仍需保存用于训练。
- 将组件接口收窄为候选、方向、目标相关输入/输出，由顶层按 `numDelay` 组合各级结果。不同级候选集可能不同，不能简单 lookup 一次后广播；组件统计和 metadata 的覆盖顺序也需保留。
- 候选须保留 taken 之前所有 not-taken 分支以更新历史；来源字段同时被 MicroTAGE 的 ABTB 过滤使用，不能当纯统计删除。
- 原始 BTB target 与最终 ITTAGE/RAS target 在训练和 fast-train 中可能用途不同，合并 target 前先检查消费者。
- 显式区分 miss 和有效 branchless fallthrough；两者都可能没有分支候选。
- 第四阶段统一公共 `BranchAttribute/BranchInfo`，让 `BranchOutcome`、PairTAGE block/packet 复用，删除 `TrainPacket::BranchFlag` 和重复块转换。
- gem5 的 conditional 可以同时 `isCond && isDirect`；RTL `BranchAttribute` 的 Conditional/Direct 互斥，迁移 accessor 时必须保留现有语义或单独验证变更。
- 保持 PairTAGE 算法和 two-taken 策略；只做所需的类型迁移，不扩展行为。

### 现有验证与证据

全部输出在 `/tmp/bpu-data-refactor/`，先确认文件仍存在。

- baseline binary：`gem5.baseline.opt`，由 base 源码重新构建后独立复制。
- 回归脚本：`run_regression.py`；统计对照：`compare.py`。
- 结果目录：`baseline/`、`stage1/`、`stage2/`。各子目录包含 `command.json/run.log/config.ini/stats.txt/result.json`。
- 4 个输入各跑 PairTAGE 开/关，配置 `configs/example/kmhv3.py`，`-I 300000`：
  - `/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin`
  - `/nfs/home/yanyue/tools/nexus-am/tests/frontendtest/cond_br_test/build/alternating_test-riscv64-xs.bin`
  - `/nfs/home/yanyue/tools/nexus-am/tests/frontendtest/br_target_test/build/return_branch-riscv64-xs.bin`
  - `/nfs/home/yanyue/tools/nexus-am/tests/frontendtest/br_target_test/build/indirect_branch_alternating-riscv64-xs.bin`
- 间接测试已有反汇编中存在 `jalr s2` 等动态跳转；普通 `indirect_branch` 不宜替代，可能优化为直接跳转。
- CoreMark 到达 300k 窗口；其他三个短程序提前正常结束。不是完整 CoreMark 结果。
- baseline 的周期：CoreMark 207449 / Pair 208126；alternating 9985 / Pair 9985；returns 8630 / Pair 8650；indirect 8829 / Pair 8829。两阶段同配置逐项相同。
- `compare.py` 比较 baseline 已存在的 stats key，排除 `host* / simFreq* / simSeconds*`；不检测额外新增 key，不能据此声称逐拍 trace 等价或完整所有配置等价。
- 阶段 2 单元测试：TAGE 30、BTB 36、ABTB 6、MicroTAGE 7、MGSC 14，共 93 项通过。日志 `stage2-{tage,btb,abtb,microtage,mgsc}.log`。
- 提交前通过仓库 style hook；最后几处修改是打印格式、include 和声明换行，未再次完整构建。新会话应先重新构建当前 HEAD。
- 注意：部分旧测试直接断言输出中的 raw counter；本阶段改为断言 `baseTaken`，减弱了 counter 精确值覆盖。后续应通过存储侧 metadata 或测试入口补回 counter 强弱/饱和检查，不能只用当前通过结论代替。
- 尚未做完整 SMT、delay 参数矩阵、逐拍 trace、RAS 全路径、独立 Astra review 或 SPEC 性能 CI。

### 可复制命令

```bash
scons build/RISCV/gem5.opt --gold-linker -j64
python3 /tmp/bpu-data-refactor/run_regression.py \
  --binary build/RISCV/gem5.opt \
  --out /tmp/bpu-data-refactor/resume-head --pair
python3 /tmp/bpu-data-refactor/compare.py \
  /tmp/bpu-data-refactor/baseline /tmp/bpu-data-refactor/resume-head

scons build/RISCV/cpu/pred/btb/test/tage.test.debug \
  build/RISCV/cpu/pred/btb/test/btb.test.debug \
  build/RISCV/cpu/pred/btb/test/abtb.test.debug \
  build/RISCV/cpu/pred/btb/test/microtage.test.debug \
  build/RISCV/cpu/pred/btb/test/mgsc.test.debug --unit-test -j24
```

不要让 unit-test 和 production SCons 在相同 build 目录并发；切换后遇到 generated enum header（如 `ByteOrder.hh`）缺失曾通过重新执行 production 构建恢复。

运行已有 bin 不需要 `AM_HOME`。本会话 shell 未设置 `GCBV_REF_SO`；回归脚本显式设置为 `/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so`。

### 最终 CI 与审查

用户已授权分阶段实现、本地验证，最后 push 分支并触发 SPEC06 0.3c CI，无需再次询问是否执行。
使用 `.agents/skills/gem5-manual-perf-trigger/SKILL.md` 与 `ci-perf-analysis`；目标 ref 的 workflow 才是参数事实来源。
已读取远端 workflow 保存为 `/tmp/bpu-data-refactor/manual-perf.remote.yml`，触发前仍需刷新。
建议选当前 `spec06-rva23-novec-gcc16-0.3c`，base/head 同配置、同 REF、同集合。用户未限定 kmhv3/idealkmhv3；本地用 kmhv3，CI 需明确选择并确保对照一致。
不要复用不同 ref/profile/参数的现有 CI 当作严格 base。最终等待真实 run 完成并比较 coverage、slice cycles/IPC 和分数；只 dispatch 不算完成。
跨模块重构完成后按用户 AGENTS 约定调用 Astra 独立 Review，提供准确 base/head 和原始需求，不先灌入主模型结论。

### 环境约束

- Git metadata 在 `/nfs/home/yanyue/workspace/GEM5_4/.git`，当前 worktree 之外；创建分支、stage/commit 需要授权的外部写入工具调用。
- 合法提交标签使用 `cpu:`；`cpu-pred:` 被仓库 hook 拒绝。
- GitHub 网络访问在 sandbox 中曾因代理 socket 禁止失败，`gh api` 用 `require_escalated` 重试可用。
- 按用户要求已停止后续开发。下一会话从这里恢复完整目标，不应把已完成两次提交当作全部完成。

## 恢复执行与 RTL 对照（2026-11-03）

- 恢复时核实分支与两次提交均与交接一致；重新构建 `849e2714bf`，8 组 `resume-head/` 对照 baseline 一致。
- 按用户要求先读本地 XiangShan RTL `b716aade5550b6c7ba21c199bc917e8275140bba`：`frontend/bpu/Bundles.scala`、`Bpu.scala` 及 BTB/TAGE/ITTAGE 的 bundles。
- 借鉴公共属性、组件私有存储/metadata、小型预测输出、顶层显式选择这几项职责；不引入通用 request/result 框架，也不复制 RTL 流水寄存器和硬件编码。
- 调整剩余阶段顺序：先完成公共描述去重，再统一阶段候选，最后按职责收窄预测接口。仍保留既有延迟、组件优先级、训练和恢复行为。

### 公共描述去重（原阶段 4）

- `branch_info.hh` 定义公共 `BranchAttribute` / `BranchInfo`；五个分类位仍独立，保留 conditional 同时 direct 及 call/return 同时存在的语义。
- `BranchOutcome` 复用 `BranchInfo`。PairTAGE block 复用同一描述，`TrainPacket` 复用 block，删除 `BranchFlag`、重复类型字段和 block 重建转换。
- 恢复 MBTB counter 的 `0 -> 1 -> 0 -> -1` 精确断言、正负饱和断言，以及 ABTB live-counter 和定向训练精确断言；只从 UNIT_TEST 接口读取预测 metadata 的存储快照。
- production 构建通过；TAGE 30、BTB 36、ABTB 6、MicroTAGE 7、MGSC 14，共 93 项通过。
- `shared-types/` 8 组对照 baseline 的已有非 host stats 全部一致。日志前缀 `shared-types-`；style 检查通过。

### 阶段候选统一（阶段 3a）

- `BranchPrediction.taken` 成为阶段方向的唯一来源；`predictedTarget` 保存最终预测目标，继承的 `BranchInfo.target` 保留原始 BTB 输入，供已有 fast-train 和训练 fallback 使用。
- 删除 `CondTakens` / `IndirectTargets` 类型、查找宏以及 `FullBTBPrediction` 的三套方向/目标映射；MicroTAGE S3 teacher 和 PairTAGE two-taken teacher 同步使用候选自身的方向。
- `FullBTBPrediction.hit` 显式区分 miss 和有效 branchless fallthrough；保留既有非空候选选级和 PairTAGE fallthrough 命中统计策略。
- 新增条件前缀历史、原始/最终目标分离、目标 override，以及 branchless/miss 的定向测试。production 构建通过；TAGE 32、BTB 36、ABTB 6、MicroTAGE 7、MGSC 14、RAS pop/push 4、uRAS 13，共 112 项通过。
- `candidates/` 8 组对照 baseline 的已有非 host stats 全部一致；日志前缀 `candidates-`。style 检查通过。
- 此步尚未收窄组件 `putPCHistory()` 接口；下一步把预测输出与顶层选择分开，完成阶段 3。

### 顶层显式合成（阶段 3b）

- 删除基类及组件的全阶段 `putPCHistory()` 接口与阶段填充 helper。BTB 提供候选，TAGE/MicroTAGE/MGSC 提供逐候选方向有效性，ITTAGE 提供逐候选目标有效性，RAS 提供返回目标，PairTAGE 提供 block。
- `DecoupledBPUWithBTB::predictComponents()` 按原组件顺序和各自 `numDelay` 显式组合。方向/目标组件每次请求先保存自己的 lookup metadata，再按阶段升序处理该阶段候选，不对候选去重或跨阶段广播方向结果。
- TAGE 无高级 provider 或选中 base 时由顶层取 `baseTaken`；MGSC 不采用修正时取预测时 TAGE 结果。MicroTAGE 只处理 ABTB 候选，miss 时显式恢复 base，保留 uBTB 候选的既有方向。
- 单元测试的阶段组装放入测试专用 `prediction_helpers.hh`，生产组件不保留旧接口适配。
- production 构建通过；TAGE 34、BTB 36、ABTB 6、MicroTAGE 8、MGSC 14、RAS pop/push 4、uRAS 13，共 115 项通过。新增 provider 有效性、不同阶段候选/metadata 覆盖顺序和 MicroTAGE native miss fallback 测试。
- `composition/` 8 组对照 baseline 的已有非 host stats 全部一致；日志前缀 `composition-`。style 检查通过。独立审查、补充 delay/SMT 对照和最终 SPEC06 CI 尚待完成。
