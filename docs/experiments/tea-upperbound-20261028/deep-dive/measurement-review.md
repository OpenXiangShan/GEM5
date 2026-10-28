# 独立测量正确性 Review（首轮原始结果）

审查对象：`/nfs/home/yanyue/workspace/GEM5_5`；性能源码 `36584adaf67da1e848d7b3a61d5ab18e84b2c12e`，报告 HEAD `47c53395350e59d1779b8744e55385aafa37d868`。本报告未接收其他助手候选、未阅读同题旧 Review，未运行模拟、未修改仓库。以下均为本轮独立核查。

**结论：当前已完成产物未发现确定的错误窗口、错误分母、不可比对照或虚假通过。尚不能验收整套新 TEA 和单轴实验：审查时只有 perlbench 基线 TEA 完成，其余运行中/未启动。**

## 实际范围

- `git status`：已修改 README、deep-dive/README、plan；未跟踪 analysis-workflow、best-config、residual-hypotheses、run_residual_probes，以及新增 astar/perlbench residual notes。未修改这些文件。
- 深读三个采集/汇总脚本；定向读取 CPU 指令计数、Simulation 继续执行、TEA dump/reset/commit/tick 路径。
- `git diff 36584ada..47c5339 -- src configs` 为空；当前 TEA 源码与性能提交内容相同。并未把报告 HEAD 当作新的性能实现。
- 实际检查所有已启动本地组的 config/log/stats；核对 CI baseline24、R4-fourload-frontend、R5-fourload-window-frontend 各三个代表点。三个 CI 归档分别有 145 completed、0 running、0 abort；没有逐项重算全部加权分数。

## E1：20M＋20M 窗口和 reset，已验证

位置：`src/cpu/base.cc:369-372,809-814`，`src/cpu/o3/cpu.cc:1457-1493`，`configs/common/Simulation.py:267-275`；`scripts/run_final_tea.py:69-73,114-128`（脚本路径均相对本实验目录）。

机制：maxinst 以线程累计 `numInst` 安排；warmup 在累计达到 20M 后调度统计 dump/reset，Simulation 收到该原因后继续；统计 reset 不把线程累计数重置为零。因此本轮 maxinst=40M 是累计总上限。每周期多指令提交会造成少量越界，脚本容忍范围与产物一致。

实际 perlbench 基线：两个完整统计块，`simInsts=20,000,011 / 19,999,994`，日志有 warmup reset 和正常 maxinst 退出。末块 `committedInsts=19,999,994`，`numCycles=4,102,519`。CI 同点完全相同。九个代表 CI 点均有两个约20M统计块、reset、maxinst 退出及 enable_difftest=true。

分类：本轮修复已覆盖此前总上限早于预热的情况；未将旧冷启动数据重新视为正式 ROI。未进行新模拟或边界微测试。

## E2：TEA 窗口守恒和 PC 分母，已验证

位置：`src/cpu/o3/tea.cc:57-103,189-213,233-299`；`scripts/summarize_tea.py:63-108,149-188`。

TEA reset 清除旧权重、pending target、drain samples 和当周期提交缓存，保留活跃指令事件及恢复原因；末次 dump 重写 CSV。四状态每次只选一个，事件位允许重叠，Computing 样本按同周期提交指令分摊。汇总以末块 committedInsts 为分母，未把事件之和当总停顿，也未伪造 PC×状态字段。

独立重新调用只读 `point()` 检查 perlbench 已完成产物：samples=cycles、committedByThread=committedInsts、四状态总和、CSV 总权重、归因/取消/pending/unresolved 以及九事件统计全部通过。末块 CPI 分量 Computing=0.1472032942、Stalled=0.0475343643、Flushed=0.0097246529、Drained=0.0006637002；unresolved=0.0010261003，pending/cancelled=0。

局限：源码核查不等于逐个验证九个事件的置位原因；未深审 fusion/microop 和异常恢复的全部路径，也没有做 SMT 扩展审查。本任务单线程 novec 已完成点不存在分母不一致的证据。

## E3：CI/本地身份与单轴对照，已验证到当前完成范围

位置：`scripts/run_final_tea.py:43-56,123-138`；`scripts/run_residual_probes.py:55-78,93-126`。

- 当前二进制 SHA256 实测 `511cb9dda739f9f39cacc768b61e0f49d3956eb4989a74be47458b69511bd923`，与本地 command.json 相符。
- 当前 REF 实测 SHA256 `e5e58a35ae758b46de9c17cf8d30ef92e0a168d197939926a7185ee94a9315f3`，与三个 CI ref-manifest、实际 INI 及本地记录相符；实际 checkpoint 路径均指向同一 260820 profile 的对应点。
- perlbench 基线与 CI 的 11,556 个共有数值字段，忽略 host 字段及两侧同时 NaN 后，无差异。这比采集脚本选定的计数集合更强，但只证明此点；CI 基线为 e34d8f6，当前 baseline 使用36584ada默认关闭新增选项，不能仅以相同配置名概括为同 SHA。
- 完整 INI：本地 candidate 相对 CI 仅 enableTea 和 DRAM 配置路径不同；baseline 还出现旧 CI 不存在的三个新增默认关闭/单供给选项。
- 两个单轴组的完整 INI 差异分别只有 `mmu.dtb.size 48→512`、`RAWQEntries 56→128` 及 DRAM 配置路径。两组所引用 CI 的 DRAM 文件与本地文件实测 SHA256 均为 `790c80e7fa4342ec2494d490606365d3ae65a0a7ca4d9a1b3defe6f5ef6c2f3f`。

分类：未发现本轮第二个参数轴或 REF 漂移。单轴文件正确标为单点，不是新 SPEC 总分。只读核查不能证明二进制来自一次无遗漏的可复现构建；已有相同源码树及已完成点数值一致证据。

## E4：准确 ELF 映射，已验证一侧热点

位置：`scripts/summarize_tea.py:111-123,135-160`。

实际对 profile 三个 ELF 重新 SHA256：mcf=`818d286599edb64b3493bb75d083cbf2bc8fb6e76a39c3da79aeb62cdf828766`，perlbench=`df8fd054a6a9c78b6058a8af595c06e6c1b654867a9c59b0a1642b3a4346721d`，astar=`73f872defdb08b3bc7a0c18a4583d87beed14dca3311fe4e623fee3052d98f52`，与现有映射记录一致。对已完成 perlbench 使用 profile ELF 反汇编：全部归因 PC 可映射；首位 `0x92e18` 为 `S_regmatch: lw a5,0(a5)`，总权重 CPI=0.0139376701。这是单侧总归因权重，未称可移除停顿或候选收益。

未深入：checkpoint 内代码字节与 ELF 的独立逐字节比对、应用源码构建 provenance、精确 DWARF 行号；这些不由脚本记录 ELF hash 自动证明。

## 必要验证缺口与已排除候选

1. **尚未完成的整组验收**：审查时 mcf 两组未有完整块；perlbench candidate、astar baseline 各只有预热块；两个单轴组尚无完整块，其余 jobs 尚未启动。完整 summary 和局部收益不能由现阶段证据宣布通过。等待完成后按现有检查验收即可，无需为本 Review 重跑。
2. **输入路径被泛化忽略（未升级为当前结果缺陷）**：`run_residual_probes.py:99-102` 将任何两侧均以 `/` 起始的差异排除于唯一参数断言，实际被排除的是 DRAM 输入文件，不是输出路径。若该输入内容不同，脚本仍可输出 passed。本轮已独立核实其内容相同，故排除“当前单轴对照实际不单轴”的候选；文档“除输出路径外”并不准确。后续复用脚本时，输入文件必须校验内容，不能把这条断言当输入身份保证。
3. **汇总脚本的检查范围**：`summarize_tea.py:57-62,149` 仅从日志/块数识别窗口并比较窗口元数据，不验证两侧 checkpoint/REF/ELF 关联或严格20M；`run_final_tea.py` 才补上本轮窗口和 CI 检查。已完成正式点符合该组合验收，未发现误用；独立运行汇总脚本打印 checks=passed 只能表示其自身守恒等检查通过。
4. **未验收全部 observer 对照**：脚本计划的 mcf两侧及perlbench candidate OFF 尚未完成。当前 perlbench baseline 对旧 CI 的精确数值一致不能代替所有候选上的 OFF/ON 等价性。

本报告为首轮独立原文；若后续收到其他助手候选，应另附“受提示复核”，不改写为独立发现。
