# GEM5 性能分析工具：问题、修复与证据台账

日期：2026-11-09。面向本轮瓶颈复盘；工具改进单独演进，不混入微架构候选。

## 分支与交付范围

- 独立分支：`perf-analysis-tools-20261106`。
- 权威工作树：`/nfs/home/wujiabin/work/GEM5-wt-tea-tools-20261104`，位于 node030。
- 本次整理前 HEAD：`8754bae19943e34b88dc82f0340bc9e8324d9d42`。
- 本轮通用工具演进基点：`883b07b3a27d201a398103235e09820e9ad354e1`。该基点已经包含 TEA sampled-state 生产者修复及基础分析工具；基点之后到上述 HEAD 的 diff 仅涉及 `util/tea/` 和 `docs/tools/topdown/tea-analysis.md`，没有 `src/`、`configs/` 或 CI workflow 修改。
- 原 best 架构分支：`fixed-resource-clean-rrip-tie-20261109`，R112 `153cd2309755352db9b0e993bdc9ef51f51735aa`。本次文档整理不修改该分支。
- 本台账在工具分支的目标路径：`docs/tools/topdown/gem5-tools-issues-and-fixes-20261109.md`。分析工作区保留同内容阅读副本。

历史基础修复有生产者代码改动，不能把整条分支相对上游的所有 diff 都说成纯离线脚本。应区分“基础观测语义修复”和“883b07b3 之后的工具演进”。两者均不记为 guest 性能收益。

## 一、通用问题与已经提交的修复

### T1. TEA 只保留 PC 边际，无法恢复采样状态联合分布

**问题与影响。** 原 `(thread, PC, PSV)` 归因与全局状态总数，不能回答某条 PC 在采样时究竟处于 Stalled、Computing 还是其他状态。延后完成、取消或跨窗口样本若丢失采样状态，容易按完成时状态误解释瓶颈。

**修复。** 基础提交 `883b07b3a27d201a398103235e09820e9ad354e1` 在 TEA 生产者中保存 sampled state，将 state 纳入 CSV 联合聚合；保留 cancelled、pending、unresolved 路径。采样状态定义和目标时序不变。相关文件：`src/cpu/o3/tea.cc`、`tea.hh`、`tests/pyunit/tea/pyunit_tea_check.py`。

**验证与边界。** 降维后复现旧 PC/PSV 边际，按状态求和复现原 samplesByState，单独保留 pending 等预算。基础语义测试位于仓库；历史验收材料见 `docs/tools/topdown/tea-analysis.md` 和分析工作区 `dip/int23-20261101/tea-state-completion.json`。没有把归因 PC 自动解释成唯一慢源。

### T2. 源码、冻结二进制和声明版本可能错配

**问题与影响。** R48 的原 manifest 声明了错误源码身份。只看分支名或模拟成功，会将真实运行归给错误候选，污染后续瓶颈比较。

**修复。** `b33e159411689f1a73f953b3096324920eb77cea` 增加 `util/tea/audit_experiment_identity.py`，绑定 manifest/build/源码 patch/冻结 binary 的身份。

**验证。** 原 R48 错配被拒绝，实际 binary/build/source 配对保留；R51 fix1、R52 正常 manifest 通过，篡改 R52 build binary digest 的负例被拒绝。记录：分析工作区 `dip/mechanisms-20261104/tools-identity-commit-20261106.json`。

**边界。** 身份一致不替代正确性、实际 INI、真实权重、REF 和官方 CI 终态审核。

### T3. 正式两版本子集缺少与完整父角一致的加权汇总

**问题与影响。** 只看切片百分比或正负点数量，会把轻权重点的大收益误判成重要收益；未测点可能被错误当成零。原四角工具还缺少真实两版本消费者所需的入口。

**修复。** 四角基础 `328ab0b802`；两版本扩展 `1e3f8f25a628bb544f9a171df3fd347df60a65fe`。`util/tea/summarize_four_corner_subset.py` 使用完整父角、原 profile 和实际退休数，输出各 benchmark 自己分母下的已测耗时贡献及未知覆盖。

**验证。** R71 真实 27 正式点逐项复现独立快照（贡献容差 1e-12 pp、份额 1e-14）；R72 真实两版本样本通过；原 R61 四角完整对象不变。短控制窗口、重复/不完整基线、失败点及非法 corner 配置均被拒绝。记录：`tools-paired-subset-result.md` 及 `tools-pair-subset-20261108/validation.json`。

**边界。** 不同 benchmark 的 pp 分母不同，不能跨 benchmark 相加；工具不生成部分点的完整套件分。

### T4. 任务结束后无法再通过 /proc 验证实际启动命令

**问题与影响。** 活进程检查只适用于在途任务；退出后如果只信任预先声明参数，仍可能漏掉实际执行的 binary 或 argv 差异。

**修复。** `068812232d85992d5298c2d5079345a48ca33fbb` 为身份审计器增加 `--completed-runs`，核对声明矩阵、成功返回、run/complete 一致、冻结 binary 路径与 run.log 中唯一实际 command line。

**验证。** R79 全部 60 fast + 16 opt 的 76 条真实日志通过；只改日志参数、实际 binary、返回码、重复 guest 四个负例分别被拒绝。默认预检输出与旧版精确相同。记录：`tools-completed-command-result.md`、`tools-completed-command-20261108/validation.json`。

**边界。** 不能恢复已结束进程的真实环境；REF/library/environment 仍须运行时记录与另外审核。

### T5. 正式新增控制采用嵌套记录，原完成审计器抛 TypeError

**问题与影响。** 正常的嵌套 complete.json 无法消费；若简单丢掉控制，则会损失精确复现证据。

**修复。** `add1d88a15ab010ceead8efe9978c109f0cd5bc0` 按 manifest 声明展开嵌套运行，并把新增长控制纳入同一唯一矩阵。

**验证。** 先复现旧 TypeError；R81 选出的 38 条真实日志和 R80 后续真实 38 条正式记录分别通过；漏控制、重复控制、未声明策略三负例被拒绝。原平铺与默认预检结果精确不变。R81 fixture 为短窗，未冒称 R80 正式结果。记录：`tools-nested-controls-result.md`、`tools-nested-controls-20261108/validation.json`。

### T6. 稀疏输出的零统计桶被误当成新增 CPU 字段

**问题与影响。** 不同策略可能改变哪些零桶被输出。直接对两个 schema 作集合差，会误报新增观察字段；反过来全局忽略差异，又会掩盖未声明字段。

**修复。** `b36bd30ffffef6d6852fae04b66a36850c6c4d69` 增加 `util/tea/cpu_observer_fields.py`，只允许明确声明的纯 host 观察字段，区分允许的稀疏桶与真正字段变化。

**验证。** 4 个语义回归、真实 GemsFDTD warmup/ROI case 与 style 检查通过；同策略控制全部共有 CPU schema/值及 CSV 的严格复现要求保留。记录：`tools-sparse-observers-commit-20261109.json`。

**边界。** 不是“凡是缺失都当零”。未知点、未知事件、旧 binary 不存在的新计数不能因此补零。

### T7. O3PipeView dispatch 时间戳为 0，不能解释排队时间

**问题与影响。** 用 0 作为真实 dispatch，会制造假的 dispatch→issue 时间；缺少地址生成记录也容易被误认为没有该依赖。

**修复。** `9efe30fc6d2378d05e2586ecee0059815f78e3d2` 增加 `util/tea/analyze_schedule_cohort.py`，从实际 Schedule `scheduler insert`、就绪、重试和调度事件关联退休 cohort。没有事件就保留未知，保留投机唤醒的负 offset。

**验证。** 3 个语义用例覆盖零 dispatch 替代、重复调度/取消、缺失与模糊 SN；4 份原 trace、两组 PC 范围共 8 次分析复现旧 cohort 数量/IQ/issue→complete 分布。没有重跑 guest。记录：`tools-schedule-cohort-commit-20261109.json`、`schedule-dispatch-analysis-complete-evidence-20261109/tools-schedule-dispatch-result.md`。

**诊断收益。** 把 Libquantum 调查从分支 RF 冲突次数进一步收窄到 load 和循环计数器的依赖就绪/排队时间。

### T8. 物理 tag 复用与融合指令使生产者关联不可靠

**问题与影响。** 分析结束时再按 tag 查询 owner，会把消费者配给后来复用该 tag 的生产者；只依赖 O3PipeView，融合地址生成指令可能缺少完整时间记录。相邻 SN 也不是依赖证明。

**修复。** `8754bae19943e34b88dc82f0340bc9e8324d9d42` 增加 `util/tea/analyze_schedule_dependencies.py`，在依赖登记时捕获物理生产者，使用真实 `add to FUs` 时间，区分原融合记录 PC 与实际 scheduled 指令。

**验证。** 4 个语义用例覆盖 tag 复用、重复依赖、融合/缺失和上下文身份；4 份原 trace 全部所选消费者数量及 Schedule/O3 issue 对齐通过。记录：`tools-schedule-dependency-commit-20261109.json`、`r115-source-start-evidence/diagnostic/physical-dependency-analysis.json`。

**诊断收益。** 确认 Lib69175 的 load 实际地址生产者，并识别循环计数器与同原分派周期 ALU 的真实选择竞争。该工具没有证明通用单源优先有效，R115 反例保留。

## 二、实验脚本/运行环境问题及纠正

这些纠正保留在原实验归档，台账统一列明；没有为一次性问题复制一套通用代码进工具分支。

| 问题 | 实际纠正 | 影响与保留证据 |
|---|---|---|
| 本地分析机与 node030 的 FS/PID 不是同一权威 | 对 node030 直接读取 `/proc/<pid>/cmdline`、源码和冻结 binary；网络经 proxychains | 本机不存在路径/PID 不被解释成远端终态；超时不作为重启理由 |
| R97 自动 runner 的 build 路径错误 | 修正专用 runner 入口，复用原已成功构建与冻结 binary | 原错误和日志保留，不因 wrapper 失败重复 build；见主进展账本 R97 条目 |
| R110 CI 审计器将不同策略的稀疏输出桶变化误报为新增字段 | 专用 auditor 复用已有 b36 的字段/稀疏桶 helper，使用 fix1 新目录重审原官方 CI | 不放宽同策略数值/CSV 或资源门禁，不重复 dispatch；旧失败保留，`r110-ci03-complete-evidence-fix1-20261109/` |
| R115 Schedule 解析把同 SN 的 STD/STA 成员当成不同动态指令 | 按实际成员身份修正一次性解析器，fix1 重放原 trace | 原 176134715 行中 122 条成员重复，恰 3 次真实反序；旧 v0 和错误日志保留，见 `r115-mcf-schedule-trace-complete-evidence-20261109/` |
| 任意反汇编区间切在压缩/变长指令中间 | 从已识别函数入口重新反汇编，绑定同 SHA ELF | 旧半条指令输出保留；`r99-dip-tea-pc-disassembly-fix1-20261109.json` |
| R26 官方 workflow 因 NFS 归档清理失败 | 原失败状态保留，对已存在完整原件独立全审；后续组合另外通过官方 success | 不能将独立原件全审改写成该 workflow 官方 success；没有修改 CI 并行度 |

上述条目是已有记录的整理。没有把所有运行失败都归咎于工具，也没有以工具修复掩盖真实微架构正确性或保护点回退。

## 三、不是代码 bug，但必须纠正的解释问题

| 容易误读的信号 | 本轮反证 | 正确处理 |
|---|---|---|
| DIP Stall 下降就代表更快 | Libquantum FrontEnd 上升更多，净 CPI 退步 | 保留完整分类抵消和净 CPI |
| TEA Computing 高等于满带宽 | Computing 只要求有退休 | 不据此宣布吞吐上限 |
| RF 冲突数下降就是根因修复 | Lib69175 更快的 off 组冲突数反而增加 | 使用受控干预与真实依赖时间，计数只作佐证 |
| dirty/writebacks 总数都能代表脏写回 | generic writebacks 包含 clean | 明确命令类型；不把流量总数当关键停顿 |
| 原轨迹中 sub 晚 33 周期就是排序损失 | 首次精确控制中原排序也 load-cancel，两侧最终 FU 时刻相同 | 同轨迹间隔与反事实延迟分开 |
| 正确性与审计通过等于因果闭合 | R114/R115 均正确，但保护点回退 | 审计证明比较可信，干预结果决定是否采纳 |

## 四、仍未解决的工具能力边界

1. DIP/TEA 归因还不能对每条关键需求自动恢复“从请求到退休”的互斥因果时间；多个请求和事件可以重叠。
2. 局部 trace cohort 不是完整 ROI；融合、缺失、取消、重复发射和边界样本仍需显式保留，不能按缺失值补零。
3. 完成后的记录不能恢复已退出进程未记录的环境；运行时身份、REF/library 记录依然必要。
4. 没有单独测得工具宿主运行时间加速，也没有对选择逻辑完成物理时序/PPA 验证。

本次整理不扩展工具功能或新增微架构实验。已有源代码修复和各自验证保留；本次提交只增加台账与入口链接，采用文档检查和链接检查，不重新运行无关模拟。

## 五、读者如何找到原件

外部证据根为分析工作区：

`/nfs/home/wujiabin/work/tea-analysis-39234318080/dip/mechanisms-20261104/`

本台账提到的 `tools-*.md`、`tools-*.json` 及实验归档路径均相对该根。仓库通用入口为 `docs/tools/topdown/tea-analysis.md`；工具实现位于 `util/tea/`，基础 TEA 语义测试位于 `tests/pyunit/tea/`。本轮可读主报告是分析工作区 `reports/experiments/gem5-bottleneck-retrospective-20261109.html`。
