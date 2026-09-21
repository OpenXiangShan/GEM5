# Decode 指令融合优化：模块交互影响评审

评审日期：2026-09-20
源码基线：昆明湖 `xs-dev` 固定提交 `ea8dc6b5d9c29accd6ec939cd75aaba9c5679465`。
优化工作树：`/nfs/home/xiongye/GEM5-decode-fusion`，分支 `feature/decode-fusion-compaction`。
文档状态：在原交互审查基础上按新基线及本轮实现更新；源码静态结论和待验证要求分开列出。
测试通过情况见[本地验证记录](decode-fusion-validation.md)及其证据，本文件不声称所有场景已经实测。

原工作树 `/nfs/home/xiongye/GEM5` 中的审查稿保留不动；不使用
`experiment/kmhv3-fetch10` 作为本轮基线。
实施边界见 [补位优化计划](decode-fusion-compaction-plan.md)。

## 一、评审结论

已确定的优化方案可以继续使用现有模块接口：

- Fetch→Decode：每拍最多8个输入条目。
- Decode→Rename：每拍最多8个输出DynInst。
- Rename→Decode：通过`blockDecode`反压。
- Decode→Fetch：通过共享阻塞信号及延迟重定向消息控制。
- Commit→Decode：传递squash，并在融合异常时设置禁融保护。
- CPU：负责阶段调度、指令列表、线程清理和drain。

优化必然改变指令到达后端的时机、融合数量及资源压力，这是预期的性能变化。需要防止的是指令丢失、重复发送、错误路径继续执行、恢复PC错误和阶段停滞。

当前结论仅针对已选范围：**单线程真实执行、输入8/输出8、新FIFO40条、扫描窗口16条、同FTQ且同loopIteration融合，load融合、ConstantFolding和MovImmElimination关闭。** 不代表所有可选配置已经兼容。

## 二、模块交互总表

| 交互对象 | 当前交互 | 优化后的影响 | 必须保持的条件 |
|---|---|---|---|
| Fetch | 向Decode发送指令包 | 消费不再受单包边界限制，反压时机变化 | 每包只接收一次；保留前向延迟；容纳在途输入 |
| Rename | 接收DynInst及停顿信息，返回反压 | 每拍融合后输出可能增加 | 输出数≤8，不能把16条raw数量写成输出数量 |
| BPU / FTQ / RAS | 经Fetch处理Decode重定向；提供返回目标 | 分支到达Decode的时机可能变化 | 保留FTQ身份、普通返回纠错、重定向优先级 |
| IEW / IQ | 执行融合对象；读取和裁剪Decode分支历史 | 队列压力、调度时机和历史推进速度变化 | 复用原融合执行语义；历史只记录真正消费的指令 |
| LSQ / 内存依赖预测 | 接收后端访存指令及路径历史 | 普通访存到达时机变化 | 本轮关闭load融合；保持历史维护及恢复处理 |
| Commit / difftest | 提交融合对象、精确异常、参考模型核对 | 融合提交数量变化 | 保留`IsFusion`、两条原始指令和正确PC跨度 |
| CPU指令列表 | 管理在途指令和清理 | 原始两项被一个融合项替换 | 替换一次；避免悬空迭代器和指令列表孤儿 |
| CPU生命周期 | tick、线程退出、drain、takeover | 新增持久FIFO状态 | 所有清理和drain入口覆盖新FIFO |
| 统计与调试 | 停顿原因、IPC、PerfCCT、O3PipeView | raw与输出数量更容易分离 | 明确计数口径；不能将扫描宽度当成输出槽宽度 |

## 三、Fetch、反压和重定向

### 1. 数据接口可以不变，但接收与消费必须分开

[Fetch发送循环](../../../src/cpu/o3/fetch.cc)每拍最多发送`decodeWidth`条。新Decode可以从FIFO消费超过8条，但不得读取尚未经过前向延迟到达的Fetch数据。

即使`blockDecode=true`，已经在途的包仍可能到达。因此“停止消费”不能等同于“停止接收”。

当前容量方案采用消费后库存`Q`计算（输入宽度8、前向延迟3、容量40）：

```text
允许Fetch发送 ⇔ Q + 3×8 ≤ 40
```

CPU 每拍先执行 Decode，再执行 Fetch。`D×W` 为 `D-1` 包既有在途输入及本拍可能
新发的一包预留空间，不依赖下一拍继续消费；入队前仍检查实际容量，禁止覆盖队头。
预留乘法使用足够宽的整数类型，避免较大非法参数先发生乘法溢出。

当前 `xs-dev` 基线增加了 Fetch 侧 predecode/resolve 流程。
新路径要求普通前向延迟至少1拍；`enablePredecode=True` 时必须至少3拍，
且不超过 TimeBuffer 后向可访问范围。保留 `isPredecodeChecked()` 守卫，
已经由 Fetch 检查的非分支错误 taken、直接分支或 return 不在 Decode 重复处理。
两条原始项均已检查时，融合项才继承该标记。

### 2. blockFetch只阻止发送，不会停止Fetch/BPU内部运行

[Fetch::tick()](../../../src/cpu/o3/fetch.cc)仍推进BPU和内部取指；共享阻塞信号在[发送候选选择](../../../src/cpu/o3/fetch.cc)等位置生效。

因此selfSquash相关的两次清理都必须保留：

1. Decode发现错误，当拍清理已经存在的年轻指令。
2. Fetch收到延迟重定向后，再清理两次处理之间生成的旧路径指令。

第二次发生在[Fetch::squashFromDecode()](../../../src/cpu/o3/fetch.cc)。不能因为新FIFO已经清空，就删除它或省略重定向消息。

### 3. 共享阻塞信号每拍都必须刷新

`StallSignals`是[持久共享对象](../../../src/cpu/o3/comm.hh)，不会随TimeBuffer推进自动清零。

以下所有出口都要完成统一收尾：空队列、后端阻塞、squash、扫描额度耗尽和正常输出。否则可能出现：

```text
上一拍：队列拥塞，blockFetch=true
本拍：队列清空，提前return，没有刷新信号
结果：Fetch继续被阻塞，无法恢复供应
```

selfSquash的强制阻塞优先于普通空间判断：

```text
blockFetch = selfSquashThisCycle || 空间不足
```

同时更新阻塞原因，避免信号与原因不一致。

### 4. 保持Commit重定向优先

[Fetch控制处理](../../../src/cpu/o3/fetch.cc)先处理Commit，再处理Decode重定向。同拍到达时，Commit优先。

新Decode只写本拍自己的输出消息，不能把squash改成未经设计的持续握手，也不能把`blockFetch`当作重定向确认。

## 四、Rename、融合对象与执行后端

### 1. Rename关心实际输出数量，不关心消费了多少raw

[Rename::moveInstsToBuffer()](../../../src/cpu/o3/rename.cc)依据`fromDecode->size`逐项接收，
按每项的线程号检查版本并分流至该线程缓冲，同时检查容量。
新 `xs-dev` 基线已支持含多个线程条目的输出包；旧稿“整包必须同线程”的描述
不适用于本基线。本轮优化仍只启用单线程路径，SMT继续走原路径。

因此必须满足：

```text
toRename->size = U
U ≤ 8
```

不能把raw消费数16写入`size`。新路径还必须遵守Rename本拍生成的`blockDecode`，不能在阻塞时先融合、弹队列，再等下一拍发送。

现有Rename按融合对象真实的源/目的寄存器处理；[寄存器需求检查](../../../src/cpu/o3/rename.cc)并未固定假设每项只有一个目的寄存器。没有发现需要因补位而扩大Rename接口的理由。

### 2. 继续复用原融合执行语义

现有融合对象在[ChainFusionInst::execute()](../../../src/arch/riscv/insts/fusion.cc)内部执行原始两条指令。优化改变“何时、哪些相邻指令形成融合”，不应另造执行语义。

融合增多后，IQ、ROB、功能单元压力及依赖链时序可能变化，这属于预期效果，不要求后端逐周期行为与baseline相同。

### 3. instList替换及原始指令所有权必须完整保留

[现有apply过程](../../../src/cpu/o3/decode.cc)将融合项插入第一条的位置，再移除原始两项。

需要区分：

- 从CPU全局列表移除原始两项；
- 销毁原始两条DynInst。

后者不能发生，因为[FusionInst保留两个DynInstPtr](../../../src/arch/riscv/insts/fusion.hh)，执行时仍需使用它们。

必须保证：

- prepare不修改全局列表。
- apply只执行一次，输出仅包含融合项。
- 替换后不再使用原始两项已失效的`instListIt`。
- 不先批量融合整个窗口，再处理其中本应触发的redirect。
- 不把被融合的原始指令当成错误路径指令标记为squashed。

## 五、控制指令、分支历史及异常恢复

### 1. pair-first前瞻不能提前执行第二条的控制副作用

例如：

```text
本拍已有7个输出
A：普通标量，将占第8槽
B：未预测的return
```

B只能被判定为不可融合，留到下一拍。如果前瞻时提前执行B的selfSquash，就可能清掉仍在FIFO内的B，却让它留在CPU全局指令列表中，造成无法到达Rename的孤立指令。

正确顺序是：**确定消费、移出FIFO、保留输出指针，然后对该原始指令执行控制处理。**

此约束同样适用于分支历史、序列化标志和Decode统计。

### 2. “控制指令不融合”并不足够，原有单项处理还必须执行

现有Decode包含以下独立行为：

- 非分支被错误预测为taken时纠错。
- 直接控制指令目标校验。
- 未预测普通return的RAS纠错。
- 非投机指令的预测状态修正。
- 分支历史记录。

这些处理位于[decodeInsts控制部分](../../../src/cpu/o3/decode.cc)，不能在新路径中简化成“不可融合就直接发送”。

普通return必须先取得保留的RAS目标、更新预测，再selfSquash。[代码顺序](../../../src/cpu/o3/decode.cc)不能颠倒。

`mret/sret`属于非投机返回，不能套用普通return的RAS路径；后续[非投机预测修正](../../../src/cpu/o3/decode.cc)也需要保留。

### 3. 同时保留版本检查和isSquashed()检查

它们覆盖不同恢复情况：

- 后端squash推进版本，用来识别迟到旧包。
- Decode selfSquash不依靠同样的版本推进，而通过指令清理设置squashed状态。

后端恢复不能简单清空整个新FIFO。按本基线的选择性squash边界，删除
`seqNum > doneSeqNum` 的年轻项及已经squashed的项，保序保留其余较老项。
幸存项更新到恢复版本，防止随后到达Rename时被新版本判定为陈旧。
恢复当拍进入Decode的较老输入按相同边界更新版本；当拍不产生新的Decode输出。
实现采用有限次数的队头弹出/幸存项尾部追加，避免循环缓冲区逐项erase造成反复移动。
正常扫描复杂度 `O(scanWidth)`，恢复筛选复杂度 `O(bufferSize)`。

两种检查不能互相替代。版本判断继续使用[SquashVersion::largerThan()](../../../src/cpu/o3/comm.hh)，不能改成普通整数大小比较，因为版本存在回绕。

两条候选都要先通过有效性检查，再查询FTQ、RAS或应用融合。

### 4. Decode分支历史还有后端调用者

它不仅是Decode内部记录：

- [IssueQueue](../../../src/cpu/o3/issue_queue.cc)向内存依赖预测传入Decode历史。
- [IEW](../../../src/cpu/o3/iew.cc)在PHAST违规处理中读取历史。
- [InstructionQueue](../../../src/cpu/o3/inst_queue.cc)等恢复路径裁剪历史。

因此新路径必须保留记录和裁剪接口。只有真正消费的分支才能加入历史，lookahead到的分支不能提前加入。

当前配置关闭PHAST，但不能因此删除这些公共交互。

### 5. Commit和difftest仍依赖融合身份

[Commit处理融合异常](../../../src/cpu/o3/commit.cc)时，会改为ReExec并通知Decode禁融。[difftest](../../../src/cpu/base.cc)对融合对象执行参考模型两步。

必须保留：

- `IsFusion`及`setFusedInst()`绑定。
- 两条原始指令的生命周期。
- 第一条起始PC和整个融合对的结束NPC。
- 禁融状态“超时首次命中仍拒绝”的行为。

更大的窗口不能改变异常拆开重跑的协议，也不能把已附带Fault的条目当作无效条目丢弃。

同FTQ和同loopIteration的准入检查之外，融合对象本身还必须显式写入这些字段；不能只检查原始两条相同而让新对象使用默认值。[Commit确实读取它们](../../../src/cpu/o3/commit.cc)。

## 六、CPU生命周期、统计与可选配置

### 1. 生命周期：需要补上clearStates()

CPU线程退出时调用[Decode::clearStates()](../../../src/cpu/o3/cpu.cc)；优化前该函数主要清理分支历史，新增实现还要清理新FIFO及来源组编号等辅助状态。

本轮要求新FIFO及其辅助状态同时接入线程清理、reset、takeover、squash、selfSquash和drain；不能只覆盖正常消费路径。

[Decode::isDrained()与drainSanityCheck()](../../../src/cpu/o3/decode.cc)在新路径中检查新FIFO；旧路径继续检查自身库存。CPU还会检查全局指令列表，因此漏改Decode检查不等于必然提前完成整个CPU的drain，但Decode自身的状态报告仍会不完整。

[CPU调度](../../../src/cpu/o3/cpu.cc)依赖阶段活动状态。FIFO仍有可处理条目时，要维持后续tick；全被丢弃、本拍无输出的情况也需覆盖。

### 2. 停顿信息是传给其他模块的接口数据

[Rename透传Decode停顿信息](../../../src/cpu/o3/rename.cc)，[IEW按向量长度逐槽累计](../../../src/cpu/o3/iew.cc)。

扫描窗口扩为16，停顿向量仍应保持8个输出槽。不能将扫描窗口宽度直接用于扩展该向量，否则会改变后端统计口径。

新FIFO可能消费多拍前到达的指令，因此不能把本拍Fetch的不足原因直接解释成所有Decode空槽的原因。新增独立指标区分接收、有效raw消费、扫描丢弃、恢复清理、融合对数、实际输出、
跨组融合、Fetch阻塞拍数、消费后占用及每拍停止原因，完整名称见优化计划。
`compactionStopReasons` 包含 `inputEmpty`、`outputFull`、`scanLimit`、
`vectorBoundary`、`serialize`、`redirect`、`backendBlocked` 和 `squash`。

正常消费应满足 `有效raw消费 - 融合对数 = 实际输出`，且有效raw与扫描丢弃之和
不超过16。恢复批量删除单列 `compactionFlushedInsts`，不能混进扫描额度。
新路径空槽统计以实际输出数为准，旧路径计数行为保留。

### 3. 架构指令数与后端对象数不能混用

[CPU::instDone()](../../../src/cpu/o3/cpu.cc)对融合体计两条架构指令、一个后端操作；[BPU提交统计](../../../src/cpu/pred/btb/decoupled_bpred_stats.cc)按收到的对象计数。

融合增加后，这些数字的差异可能扩大，不代表丢失指令。跑分仍使用架构指令口径，不以`committedOps`代替。

### 4. Trace模式与真实执行下的调试追踪是两件事

SMT和Trace输入模式走旧路径，并不意味着新路径的O3PipeView或PerfCCT自动正确。

新DynInst的[fetchTick默认值为−1](../../../src/cpu/o3/dyn_inst.hh)，
而[O3PipeView输出](../../../src/cpu/o3/dyn_inst.cc)依赖它。本轮明确保留原始两条指令的
追踪记录，不直接给融合项复制第一条fetchTick，否则可能生成重复seqNum的Fetch记录。
真正消费原始条目时更新其Decode时间；融合关系另外通过Decode调试消息记录。
这取代旧稿“直接复制融合项fetchTick”的建议。

PerfCCT按原始指令建立元数据；融合复用第一条seqNum后，图中可能仍显示第一条的反汇编。现有行为不应被误当成融合关系已经完整可视化。本轮保持此既有表示，不创建新的融合来源可视化；不得通过重建PerfCCT元数据覆盖已有Fetch历史。工具解析/查询入口仍须验证，该限制必须记入结果说明。

### 5. 可选配置存在既有兼容风险

优化路径启用时，启动检查拒绝 `enable_loadFusion`、`enableConstantFolding` 和
`enableMovImmElimination` 为True的组合。普通MoveElimination及普通load值预测
继续遵循原配置，不额外禁用。False、SMT和Trace旧路径不施加新路径专用限制。

静态检查发现：[融合类合并两条静态flags](../../../src/arch/riscv/insts/fusion.cc)，例如`lui+addi`可能继承`IsAddImm`；[Rename常量折叠](../../../src/cpu/o3/rename.cc)则按普通立即数加法处理，而融合类未提供对应的复合立即数。这是需另行验证的已有配置组合风险，不能由本轮默认配置的通过结果证明兼容。

load融合继续关闭。未来开启时，要追加LSQ、MMIO、跨页、未对齐和load值预测验证。尤其[值预测已经在Fetch发生](../../../src/cpu/o3/fetch.cc)，不能假设所有预测记录都在Decode之后才产生，进而随意丢弃或复制原始load的预测元数据。

## 七、本轮实现必须满足的交互要求

以下要求已纳入本轮实现/验证计划；通过情况需由实际测试确认：

1. 所有提前返回路径统一刷新`blockFetch`及阻塞原因。
2. 明确保留Fetch收到redirect后的第二次指令清理。
3. 将直接分支、普通return、mret/sret和非投机预测修正逐项列入单项路径。
4. 同时检查版本和squashed标记，版本比较保留回绕语义。
5. 新FIFO接入`clearStates()`及线程退出流程。
6. 明确apply后的原始DynInst所有权和失效迭代器约束。
7. 保留分支历史的后端读取、记录和裁剪交互。
8. 明确停顿向量仍为8槽，调试时间戳单独处理。
9. 将常量折叠、load融合、MovImmElimination组合通过启动检查明确拒绝。
10. 保留新基线predecode已检查标记和至少3拍的必要延迟约束。
11. 后端squash按序号保留较老幸存项并更新版本，不把原基线的选择性清理改成整队列清空。
12. 从xs-dev固定提交构建A/False/True三组对照，不混入其他实验分支。

另外，同FTQ等新保护可能拒绝旧实现原本接受的部分配对。因此最终分数是“新队列、补位算法及配对约束”的净效果，不能全部解释为单独删除fixedbuffer的收益。

## 八、交互回归验收表

| 场景 | 要确认的结果 |
|---|---|
| Rename连续阻塞后恢复 | 无提前消费、无重复输出、无Rename缓冲溢出 |
| Fetch存在在途包时后端阻塞 | FIFO容量安全，包不丢失 |
| 队列拥塞后清空 | `blockFetch`正确解除，无永久停滞 |
| selfSquash后Fetch继续内部取指 | 延迟redirect覆盖期间新产生的旧路径指令 |
| Commit与Decode重定向同拍到达 | Commit优先，无旧redirect复活 |
| 第8个输出后紧跟未预测return | return下一拍才处理，不被前瞻误清除 |
| 普通return、mret/sret | RAS与非投机处理分别正确 |
| 同版本selfSquash迟到包、旧版本后端迟到包 | 两类均被正确识别和丢弃 |
| 后端squash时队内和当拍输入包含较老幸存项 | 按doneSeqNum保序保留，正确更新版本，恢复当拍不输出 |
| Predecode开关及前向延迟边界 | 不重复处理已检查的指令，不允许未满足3拍要求的配置 |
| 同FTQ跨Fetch组融合 | PC、FTQ、loopIteration、提交和difftest一致 |
| 融合执行异常 | 拆开重跑，异常PC和架构状态正确 |
| 队列非空时线程退出、drain、takeover | 无残留、无提前完成、无停止推进 |
| 停顿统计及可选调试开启 | 维持8槽口径，时间戳与实际输出可解释 |
| 优化关闭、SMT、Trace | 原路径继续工作，未切入另一套库存 |

这些测试属于本轮功能交互验证，当前不触发完整0.3c性能CI。
后续明确采用三组：原始xs-dev提交A的 `baseline-for-decodeinst`，优化提交B的
`fause-for-decodeinst`（参数False），以及同一B的 `true-for-decodeinst`（参数True）。
A/False验证旧路径，False/True比较优化净效果；保存同一测试集、工作流、REF、配置和权重，
核对145份有效checkpoint后再报告总分、INT/FP及全部29个子项。
任何功能用例未执行或环境受限必须如实记录，不由总分相近代替。

**最终判断：现有接口能够承载该优化，主要修改可以限制在Decode及其配置和测试中。当前方案的风险集中在重构时遗漏控制副作用、提前处理前瞻指令，以及新FIFO未接入完整生命周期。静态审查尚不能替代实现后的正确性验证。**
