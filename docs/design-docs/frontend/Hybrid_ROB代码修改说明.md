# Hybrid ROB 压缩代码修改说明

本文解释提交 b2088a2（cpu-o3: Add Hybrid ROB compression）的代码改动，面向不熟悉 C++、gem5 和处理器内部结构的读者。附件《Hybrid 更改完的恢复文档》只作为已有实现和验证结果的背景资料，不是新的修改指令。

## 1. 先理解基本概念

**DynInst** 是 gem5 用来表示“一次动态执行”的对象。它包含指令类别、序号、执行完成状态、异常状态、是否被取消等信息。循环中的同一条机器指令，每次执行都会产生不同的 DynInst。

**ROB（Reorder Buffer，重排序缓冲区）** 是按程序顺序保存 DynInst 的队列。指令可以乱序执行，但必须在 ROB 队首按顺序退休（commit），这样程序对外仍表现为顺序执行。分支预测错误或异常发生时，ROB 删除较年轻的指令，这个动作叫 squash。

**ROB group** 是本次 Hybrid 模型使用的物理容量单位。普通模式通常一条指令占一个单位；Hybrid 模式允许多条指令放进一个 group。threadGroups 记录每个 group 还剩多少成员，所以 8 条可合并指令可能只消耗 1 个物理 group。

Hybrid 先把指令分成三类：S（Simple，普通算术）、C（Complex，访存或控制）和 N（NoCompress，不能压缩）。分类优先级是 N > C > S：只要指令带异常、屏障、原子或其他特殊属性，就优先归 N，单独成组。

## 2. 修改文件总览

| 文件 | 作用 | 这次修改 |
|---|---|---|
| [BaseO3CPU.py](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/src/cpu/o3/BaseO3CPU.py) | Python 参数和枚举定义 | 新增 Hybrid 策略和退休配额 |
| [rob.hh](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/src/cpu/o3/rob.hh) | ROB 类声明、数据结构、接口 | 声明分类、分组计划、批量插入和统计 |
| [rob.cc](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/src/cpu/o3/rob.cc) | ROB 的实际运行逻辑 | 实现分类、规划、容量、插入和恢复检查 |
| [commit.hh](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/src/cpu/o3/commit.hh) | Commit 类声明 | 保存新参数和统计项 |
| [commit.cc](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/src/cpu/o3/commit.cc) | 提交、退休和 Rename→ROB 准入 | 接入批量分组和退休上限 |
| [kmhv3_hybrid.py](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/configs/example/kmhv3_hybrid.py) | KMHV3 运行配置 | 提供可复现的 Hybrid 实验入口 |
| [SConscript](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/src/cpu/o3/SConscript) | SCons 构建规则 | 注册 Hybrid gtest |
| [rob_hybrid.test.cc](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/src/cpu/o3/rob_hybrid.test.cc) | C++ 单元测试 | 测试分组规划器边界 |
| [hybrid_method_test.py](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/src/cpu/o3/hybrid_method_test.py) | 方法级测试驱动 | 编译并测试真实生产方法 |
| [hybrid_trace_check.py](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/src/cpu/o3/hybrid_trace_check.py) | 日志和统计检查 | 从 trace 重建 group 并验证守恒 |
| [hybrid_exception.S](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/src/cpu/o3/testdata/hybrid_exception.S) | RISC-V 异常小程序 | 验证精确异常和 squash 恢复 |
| [README.hybrid.md](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/src/cpu/o3/README.hybrid.md) | 使用和验证说明 | 记录命令、结果和限制 |

## 3. 配置层修改

### BaseO3CPU.py

第 71～72 行：

    class ROBCompressPolicy(ScopedEnum):
        vals = [ 'none', 'kmhv2', 'MohBoE', 'kmhv3', 'hybrid' ]

ScopedEnum 是“只能从列表中选择”的字符串枚举。加入 hybrid 后，配置文件才能写 cpu.RobCompressPolicy = 'hybrid'。默认值仍为第 284 行的 kmhv2，所以旧实验不会自动切换。

第 168 行把 commitWidth 的描述改为 Physical ROB group commit window。数值含义从“指令数”澄清为“物理 group 数”：压缩后一个 group 可能包含多条 DynInst。

第 169～171 行新增 commitInstWidth。它是非负整数，默认 0；非零时限制每周期成功退休的普通 DynInst 数，0 表示保持原来的 group-window 行为。

### kmhv3_hybrid.py

这是新建的实验配置。setKmhV3HybridParams 先调用标准 KMHV3 参数，然后设置：

    cpu.numThreads = 1
    cpu.valuePred = NULL
    cpu.enable_loadFusion = False
    cpu.enableConstantFolding = False
    cpu.enableMoveElimination = False
    cpu.enableMovImmElimination = False
    cpu.RobCompressPolicy = 'hybrid'
    cpu.CROB_instPerGroup = 8
    cpu.renameWidth = 8
    cpu.commitWidth = 8
    cpu.commitInstWidth = 16

这些行分别表示：单线程；关闭 value predictor、Load fusion、常量折叠和两种 Rename 消除；选择 Hybrid；每组最多 8 条；Rename 每次最多送 8 条；每周期最多访问 8 个物理 group；每周期最多成功退休 16 条 DynInst。标准 KMHV3 的 BPU、缓存和其他系统参数仍保留，便于比较时只改变 ROB 实验因素。若命令行请求 SMT，配置直接报错，因为当前 Hybrid 实现只支持单线程。

## 4. ROB 声明：新增哪些数据结构

### rob.hh 的类型

第 93～110 行定义最终 group：

    enum class HybridGroupType {
        NormalS, NormalC, NormalN, CC, CS, SC, NumTypes
    };
    struct HybridGroup {
        unsigned length;
        HybridGroupType type;
    };
    using HybridPlan = std::vector<HybridGroup>;

NormalS/C/N 表示单一类别；CC、CS、SC 分别表示 C+C、C+S、S+C；NumTypes 只是统计数组大小，不是实际类型。HybridGroup 保存一个 group 的长度和类型；HybridPlan 是一批待插入 group 的临时列表。

第 112 行定义输入分类：Simple、Complex、NoCompress。

第 114～158 行的 appendHybridClass 是一个小状态机。它每次只看计划的最后一组：

1. 最后一组是 NormalS 时，遇 S 增加长度，遇 C 改成 SC。
2. 最后一组是 NormalC 时，遇 S 改成 CS，遇 C 改成 CC。
3. CS 后面还能继续放 S。
4. N、已封闭的 SC/CC、达到 group_limit 等情况都会新建长度为 1 的 group。
5. assert(group_limit > 0) 防止出现零容量 group。

每次只处理最后一组，单条操作是 O(1)，整批规划是 O(renameWidth)。

第 180～184 行增加 hybrid 标志、分类函数、公共插入函数和不变量检查函数。第 223～232 行增加 isHybrid、planHybridBatch 和 insertHybridBatch：先规划和检查容量，再真正修改 ROB，容量不足时不会插入半批数据。

第 486～505 行增加统计字段：成功分配的 group 数、DynInst 数、六种 group 类型分布、group 长度分布，以及 DynInst 数除以 group 数的压缩率。

## 5. ROB 实现

### classifyHybridInst

rob.cc 第 134～176 行按 N > C > S 分类。

第 139～147 行检查 faulted、序列化、非投机、squash-after、读写屏障、原子、LoadReserved、StoreConditional、向量、微操作和宏操作等状态。满足任意条件就返回 NoCompress。

第 150～153 行检查 Load、Store、Control 和 IntJpOp，返回 Complex。当前解码语义下 AUIPC 属于 IntJpOp，所以归 C。

第 156～170 行列出允许归 S 的整数和浮点 OpClass：IntAluOp、IntMultOp、IntDivOp、Int2FpOp，以及 FloatAdd/Mult/MultAcc/Div/Sqrt/Cmp/Cvt/Mv/Misc。没有列出的类型走 default，打印调试信息并保守归 N，避免未知指令被错误压缩。分类使用 DynInst 的解码结果，因此 RVC 和被解码成 IntAluOp 的 PREFETCH hint 也按当前语义处理。

### 规划和批量插入

planHybridBatch（第 178～190 行）遍历 Rename 窗口中的有效指令，确认指令存在、未 squash、属于线程 0，然后调用分类器和 appendHybridClass。它只产生 length/type 计划，不修改 ROB。plan.size() 就是本批次需要的新物理 group 数。

insertHybridBatch（第 192～227 行）先确认 plan.size() 个 group 有空间，再按计划逐条调用公共插入函数。调试构建会检查所有 group 长度之和等于输入 DynInst 数；全部插入成功后才更新统计，因此失败批次不会留下假数据。

assertHybridInvariants（第 229～257 行）检查 group 数、DynInst 列表和总成员数是否一致，且每个 group 长度都在 1 到 CROB_instPerGroup 之间。开启 ROB debug 时再完整求和，用日志验证“各组成员总数 = ROB 中 DynInst 总数”；完整扫描不在正常热路径中执行。

### 构造、容量和恢复

ROB 构造函数（第 259～320 行）根据参数设置 hybrid。Hybrid 启动时若发现多线程、value predictor、Load fusion、常量折叠、Rename 消除或 group 大小为 0，就用 fatal_if 停止，因为这些组合超出当前模型假设。

策略分派（第 342～363 行）保留旧的 allocateGroup_none/kmhv2/MohBoE/kmhv3。Hybrid 将回调设为空指针，表示由批次计划明确决定边界，不再逐条调用旧 allocator。

canAllocate（第 535～556 行）和 numFreeEntries（第 803～818 行）在 Hybrid 下按物理 group 数判断容量。比如只剩一个 group，但 8 条 S 能合成一个 group，就允许整批进入；若计划需要两个 group，就必须等待。

原 insertInst 被拆成普通入口和 insertInstWithGroup（第 621～678 行）。旧模式继续自行判断是否开新组；Hybrid 直接传入 new_group。公共函数仍负责链表、head/tail、setInROB 和总计数，减少重复代码。

退休、清理 squash 和 doSquash 都增加不变量检查。doSquash（约第 871～894 行）专门处理删除最后一条 DynInst 时没有前驱迭代器的情况，并更新 ROB head/tail，避免异常恢复访问失效链表。

## 6. Commit 阶段修改

### commit.hh

第 525～532 行将 commitWidth 说明改为物理 group 窗口，并新增 commitInstWidth 成员。第 671～673 行新增 commitInstWidthFullCycles，记录成功退休数恰好达到配额的周期数；它不表示一定还有其他就绪指令被阻塞。

### commit.cc 的退休配额

构造函数（第 155～156 行）读取 params.commitInstWidth；统计构造函数（第 301～304 行）注册新计数器。

commitInsts 主循环第 1543～1548 行规定：非 squash 指令达到 commitInstWidth 后停止继续正常退休；squashed 指令不消耗该配额，仍可排出 ROB，以免异常恢复被卡住。commitHead 成功后第 1591～1597 行在“刚好达到配额”的瞬间记统计；第 1934 行的断言保证实际退休数不超过配额。配额为 0 时这些限制关闭。

### moveInstsToBuffer 的批处理

第 2355～2372 行从 fixedbuffer 队首取最多 renameWidth 个位置。先过滤 squash 对象，但不从后面补新指令；这样窗口边界稳定，失败重试时可以重新规划同一批。

第 2385～2387 行和 2442～2451 行把准入检查从“指令数量”改成 required_groups = hybrid_plan.size()。容量不足时保留 fixedbuffer 内容，不会错误消费。

第 2454～2465 行在 Hybrid 下调用 insertHybridBatch，成功后一次性弹出原窗口的所有位置并更新最年轻序号。后面的 else 仍是旧模式逐条插入代码，因此四种旧策略行为不变。

## 7. 测试和验证文件

### SConscript

新增 GTest('rob_hybrid.test', 'rob_hybrid.test.cc')，告诉 SCons 构建 Hybrid 规划器的 GoogleTest 程序。

### rob_hybrid.test.cc

该测试直接调用生产代码的 appendHybridClass。5 组测试覆盖指定 S/C/N 示例、六种最终 group 类型、组长度上限、空批次和批次独立性，并穷举长度 0～8 的所有 S/C/N 序列，在限制 1、2、3、8 下检查分组合法。

### hybrid_method_test.py

脚本提取工作区内真实的 C++ 方法体，用最小的 DynInst、ROB、CPU 和 Commit 替身编译运行。因此不会因测试代码复制实现而掩盖生产代码错误。7 项测试覆盖分类优先级、只剩一个空闲 group 时 8 条 S 的插入、容量不足时不消费和不统计、squash 后重规划、全 squash/混合窗口不补位、部分退休和尾部 squash。它仍不替代真实解码、执行和 Rename 恢复测试。

### hybrid_trace_check.py

脚本读取 rob.log、config.ini 和 stats.txt，依次：

1. 从 Hybrid allocated group 日志建立 group 队列。
2. 从退休、squash 和 CommitRate 日志减少对应 group 的剩余成员。
3. 检查 group 容量、commitWidth 窗口和 commitInstWidth 上限。
4. 对比分配组数、DynInst 数、类型和长度统计。
5. 检查守恒式：已退休 + 已 squash + ROB 剩余 = 曾分配 DynInst 总数。

rates-only 模式只检查提交速率和退休配额。

### hybrid_exception.S

这是一个 RISC-V 汇编小程序。它清除 mstatus.FS 后执行 fadd.s，触发执行期浮点异常；同时包含 ecall、trap handler 和 mret。这样可以观察异常时年轻指令是否被精确 squash，以及 group 成员计数是否正确恢复。

### README.hybrid.md

该文件是操作手册，记录编译、CoreMark/NEMU、异常测试命令、统计结果和模型限制；本文负责解释这些代码为什么存在。

## 8. 验证结果和限制

附件记录的验证结果为：5 个 gtest 和 7 个生产方法测试通过；Hybrid16 的最大成功退休数为 16，Hybrid0 保持旧的无限额行为；CoreMark 和异常 NEMU difftest 通过；none、kmhv2、MohBoE、kmhv3 四种旧策略回归统计一致。

全量单元测试在 6 个已有 socket 测试处因沙箱禁止创建 socket 而停止；SPEC checkpoint 和专项中断压力测试未执行。当前模型没有新增真实 RAB、slot 或流水级，也不宣称与 XiangShan RTL 或论文达到周期级性能等价。融合 DynInst 可能代表两条架构指令，所以 ROB 分配统计与 committedInsts 的统计口径可能不同。

默认策略仍是 kmhv2。只有显式使用 RobCompressPolicy = hybrid，新的批量分组、物理 group 容量和退休配额语义才会生效。

## 9. 性能 CI 入口补充

后续还在 [manual-perf.yml](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/.github/workflows/manual-perf.yml) 的配置选项中加入了 kmhv3_hybrid.py，这样手动性能测试可以直接选择 Hybrid 入口，而不必误用普通 kmhv3.py。

普通的 kmhv3.py 也支持通过已有的 --param 接口选择 Hybrid：

    --param='system.cpu[0].RobCompressPolicy=hybrid'

kmhv3.py 检测到这个参数后，会自动设置 Hybrid 所需的单线程、关闭 value predictor/Load fusion/Rename 消除、group 大小和提交参数；后续显式的 --param 设置仍可覆盖这些默认值。

[gem5-perf-template.yml](/nfs/home/kongqiuyuan/workspace/GEM5_20260909/GEM5/.github/workflows/gem5-perf-template.yml) 增加了 --param 参数的启动前检查。若输入被截断为 --param=system.cpu[0].numThrea 这类没有“参数值”的字符串，CI 会在启动 workload 前立即报出格式错误；否则每个 checkpoint 都会启动一次 gem5，再分别产生 KeyError，导致大量无关的 abort 文件。
