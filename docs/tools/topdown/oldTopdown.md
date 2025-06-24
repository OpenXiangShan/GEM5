# 老版本topdown计数器

## 1 自底向上性能计数器（基本计数器）
基本的性能计数器已经完备，还需要补充的是在各级缓存中区分一个请求的来源和类型。

二级和三级缓存的Req count（应该是A通道？）和Miss count至少要区分开一级、二级的预取器和来自CPU的请求

|  | L1数据预取 | L1指令预取？ | L2预取 | CPU数据 | CPU指令 | PTW请求 |
| --- | --- | --- | --- | --- | --- | --- |
| Req count |  |  |  |  |  |  |
| Miss count |  |  |  |  |  |  |




为了实现上述目标，需要让L1 告诉L2读请求的类型，L2也告诉L3读请求的类型。

因此，从L1发送到L2的请求，应该用一个变量区分**CPU**请求和**L1预取器**请求（writeback的请求暂时视为**CPU**）。需要一级缓存做出相应的调整@DCache负责人。

如果现在ICache已经开启FDIP，那么L1 ICache也需要区分**CPU**请求和**L1预取器**请求。如果没有开启就暂时不管。@ ICache负责人

L2会接收到来自L1I、L1D、PTW的请求，来自L1的请求可能是CPU，也可能是预取器；L2需要**根据请求来源**区分CPU指令、L1指令预取、CPU数据、L1数据预取。类似的，在L2区分的类型的基础上，L3还需要增加区分来自L2预取器的请求类型。@huancun负责人



建议：由@ huancun负责人定义一个全局的enum，用于区分上述类型的请求，以供L1的同学使用。

注意：上述Req count和Miss count 的统计与topdown、MLP没有关系。就是传统性能计数器的统计方法，只需要增加请求来源类型的区分。

## 2 自顶向下性能计数器
### 2.1 顶层计数器定义
每个流水级都需要统计产生空泡或者阻塞的原因，当指令供应不足时向下一级流水线报告原因。最终，这些原因在分发（Dispatch）阶段进行加和。

定义：

Slots：周期 x **分发宽度w**，它的含义是每一个周期都有分发w条指令的机会，这些机会要么被利用了，要么被浪费了。

StallReason：阻塞或者空泡的原因。

计数器尽量均按照slots而非周期来计算

计数器命名

1. 采用首字母大写的驼峰法
2. 反压浪费的slots称为Stall，例如LoadL1Stall;指令供应不上浪费的slots称为Bubble，例如FetchFragmentationBubble，ICacheMissBubble



顶层计数器定义为4类：

1. Base slots：最终提交的指令数量。
2. Bad speculation slots: 推测错误浪费的slots，分为三部分
3. Backend stall slots:流水线后端阻塞导致无法分发的slots，分为memory bound和core bound
4. Frontend stall slots: 前端指令供应不足导致的slots。分为latency bound 和bandwidth bound



包括itlb miss，icache miss，BTB  miss导致的某个完整的周期无法供应指令形成的气泡，并且气泡流动到了分发级。此外，还有指令供应碎片化导致的不完整的气泡（例如一个周期6个slots只用了3个），这可能是因为基本块横跨cache line导致截断或者基本块过短导致的。

### 2.2 Slot归因原则
#### 2.2.1 **记录始作俑者**
假设有A B C 3个流水级，在某个周期

1. 如果B往C送了>= w指令，那么该周期的6个slot都视为Base slots。
2. 如果B往C送的指令n <w，那么记为w-n个Fragmentation Bubble。在GEM5里Fragmentation一定是因为基本块较短或者截断造成的。在香山里可能会因为dispatch queue出现额外的截断，所以可能需要区分Fetch Fragmentation和Dispatch Fragmentation。如果都有fragment，如何判断优先级？假定后者高于前者
3. 如果B没有往C送指令，有两种情况，6个slot 全部记录为某个Stall
    1. B收到了来自C的阻塞，例如ROB回滚、PRF不足。此时B应该告诉C该周期的空泡是因为被C阻塞导致的。在C阻塞B时，应该告诉B阻塞的具体原因StallReason，B需要把StallReason  echo回C
    2. A没有供应指令给B，例如icache缺失。此时B一定收到了A发过来的StallReason，那么B应该把A发的StallReason原样送给C

#### 2.2.2 Stall优先级判定
有可能出现同一个周期没有指令分发时，前端和后端同时找到了各自的原因。例如分支预测错误后，前端会出现重定向气泡（RedirectBubble），后端同时会进行ROB回滚（ROBWalk），导致指令无法重命名、分发。在CPI stack中，只能体现一种，目前模拟器采用的策略是**后端优先、覆盖掉前端**，RTL请按照相同的规则处理。

这样的统计方法会存在问题，导致性能瓶颈在CPI stack中被掩盖。为了后续分析方便，我们需要保留原始的性能计数器件，即保留RedirectBubble和ROBWalk的未经覆盖的计数器。

### 2.3 流水线前端负责部分
![](images/1729064054907-e8bdb465-c6a1-4c1c-b973-26571ed14e80.png)

上图是原始topdown中的frontend bound分类，在Decoupled frontend中还需要增加override产生的气泡。所以，当一个周期无法供应指令时可能有多种原因：

1. **ICacheMissBubble**
2. **ITLBMissBubble**
3. 重定向（我们称为Redirect Bubble，原始的topdown论文称之为Branch Resteer），在我们定义的topdown中重定向的损失属于Bad speculation slots，但是在前端进行统计、并送到分发段处理。建议命名为**RedirectBubble。**为了处理多种导致redirect的原因，我们将RedirectBubble细分为**ControlRedirectBubble**，**MemVioRedirectBubble**和**OtherRedirectBubble**（中断、异常、特殊指令冲刷）。

其中，**ControlRedirectBubble**又在前端被细分为**TAGEMissBubble**、**SCMissBubble**、**ITTAGEMissBubble**、**RASMissBubble**及**BTBMissBubble**（即归类到下一类中），不记录整体的**ControlRedirectBubble**。

1. BTB miss导致预测错误，在decode stage检查出了错误，定义为**BTBMissBubble**
2. 多阶段预取器的第N+1阶否决了第N阶的结果，产生气泡，定义为**OverrideBubble**。后续如果有需求可进一步细化。
3. Fetch收到后面的流水线的信号，要求阻塞，此时原样返回从后面的流水级送来的StallReason

此外，当一个周期供应的指令数量为n，n<w时，记为w-n个slot的**FetchFragBubble**



在大部分情况下decode stage只需要把fetch送来的Stall Reason forward给rename即可。

需要额外处理的情况是

1. Decode检查出了预测错误，记为**BTBMissBubble**
2. Decode收到后级流水线的阻塞信号，原样把阻塞信号送给后级流水线

### 2.4 流水线后端负责部分


后端需要负责Base slots ，Backend stall slots和Bad speculation slots的统计。其中base slots较为简单，就是提交的指令总数。



当无法分发指令时，

1. 如果正在进行推测错误恢复，那么归因到bad speculation
2. 如果是ROB/PRF/LQ/SQ/Issue Queue/Dispatch Queue发生了阻塞，
    1. 如果是Dispatch Queue发生了阻塞，且此时ROB head并非与DQ类型相对应的指令（对于LsDq，还要求SQ未阻塞），那么归因于对应的Dispatch Queue Bound
    2. 如果是SQ阻塞，那么归因于Store Bound
    3. 如果是其它阻塞，看ROB head，如果ROB head是store指令，那么也归因于Store Bound；如果是load指令，那么归因于Load bound；如果是其他指令，那么归因于Core bound

#### 2.4.1 Memory bound 统计
对ROB head 的load指令或者SQ head 的store指令，阻塞原因可以划分为如下的矩阵，需要根据指令的状态进行细分。

如果访存指令没有完成翻译，那么需要归因到TLB 或者PTW。考虑到统计成本，目前我们暂时不区分L1 DTLB、L2 TLB和PTW，因为这部分的性能损失一般占比不大。

因 TLB 缺失导致的 load 重发还可以进一步分为以下两类：1）此时 TLB 并未处于缺失状态，Load Replay Queue 中的重发逻辑并未重发 TLB 缺失的 load 请求，导致 TLB 相关的阻塞。我们称为 LoadTLBReplayStall。2）TLB 确实发生了缺失，此类阻塞称为 LoadTLBMissStall。

这两类 bound 加和的结果为TLB 的总bound，记为 LoadTLBStall。



| | 大类 | 描述 | Load | Store（暂时不用细分） |
| --- | --- | --- | --- | --- |
| TLBStall | DTLB Replay | LoadTLBReplayStall |  | |
| | TLB miss | LoadTLBMissStall |  | |
| |  | 以上两项合并为LoadTLBStall |  | |
| ScheduleStall | Violation replay | LoadVioReplayStall |  | |
| | MSHR replay | LoadMSHRReplayStall |  | |
| CacheStall | L1 Cache | **LoadL1CacheStall**<br/>**（注意：bank conflict stall 被包含在这里）** |  | |
| | L2 Cache | **LoadL2CacheStall** |  | |
| | L3 Cache | **LoadL3CacheStall** |  | |
| | Memory | **LoadL4CacheStall** | 统一合并为StoreStall | |




A拓展的指令统一合并为AtomicStall

如果访存指令完成了翻译，需要归因到各级缓存，此时需要缓存进行支持，即告诉core某个访存请求是否正在被响应、是否正在miss。

这里给一个Naïve的思路：用Chisel的飞线功能（addSink，addSource），从core把head 的地址发到各级缓存。各级缓存返回该请求有没有发生miss。此处的飞线会一并包含 hartid 以支持 TopDown 在多核香山中使用。

然后core根据如下方式进行判断：If not ready: MemNotReadyStall

Elif load replay queue 报告因 TLB miss 的重发，但是 TLB 不处于缺失重填状态：LoadTLBReplayStall 

Elif load replay queue 报告因 TLB miss 的重发，TLB 处于缺失重填状态：LoadTLBMissStall

Elif load replay queue 报告因 load 违例进行的等待：LoadVioReplayStall

Elif load replay queue 报告因 MHSR 请求失败进行的等待：LoadMSHRReplayStall

Elif not L1 miss: **LoadL1CacheStall**（bank conflict replay included here）

Elif not L2 miss: **LoadL2CacheStall**

Elif not L3 miss: **LoadL3CacheStall**

Else: MemStall

访存统计的优先级为：atomic>store>load

##### 2.4.1.1 访存违例的统计方法
流水线后端（@thj）通过addSource广播head instruction的虚拟地址和lqidx。访存组（@whq）通过addSink接收虚拟地址，在replay queue中进行查询，如果命中，则返回True，以及Replay的原因（是/不是访存违例）。

同理，此处addSource/addSink全部加上HartID

#### 2.4.2 Core bound 统计
1. Core bound主要有以下情况：
2. 整数DQ，归因于IntDqStall，看IntDq是否阻塞且ROB head 是否**不**为整数指令
3. 浮点DQ，归因于FpDqStall，看FpDq是否阻塞且ROB head 是否**不**为浮点指令
4. 访存DQ，归因于LsDqStall，看LsDq是否阻塞、ROB head 是否**不**为浮点指令且SQ是否并**未**阻塞
5. 除法指令(int div，fp div/sqrt)，归因于DivStall，看ROB head是否为除法指令
6. 整数，归因于IntNotReadyStall, 看ROB head 是否为整数指令
7. 浮点，归因于FPNotReadyStall，看ROB head是否为浮点指令
8. 访存，归因于MemNotReadyStall，看ROB head是否为访存指令或SQ是否阻塞

#### 2.4.3 Bad speculation统计
我们弃用了原始topdown中的bad speculation分类，在只考虑分支预测错误的前提下，我们将其分为

1. **RedirectBubble**，由frontend统计，在分发级进行加和
2. bad spec recovery，推测错误后恢复rename map时阻塞的周期数以及redirect当周期，可能需要在rename stage统计，送到分发阶段。建议命名为**ROBRecoveryStall。**为了处理多种导致冲刷的原因，我们将**ROBRecoveryStall**细分为**ControlRecoveryStall**，**MemVioRecoveryStall**和**OtherRecoveryStall**（中断、异常、特殊指令冲刷）。
3. flushed Insts，只需要在最终结束仿真时计算，用分发的指令数减去提交的指令数，建议命名为**FlushedInsts。暂不进一步细分。**
4. 访存推测错误的处理

在用squash进行访存推测错误的体系中，可以区分分支预测错误和访存推测错误造成的Redirect Bubble和bad spec recovery，但是flushed Insts较难区分，可以暂时合并统计。

在用replay进行访存推测错误恢复的体系中，上述统计方法无法统计replay造成的性能损失。目前我们使用 LoadVioReplayStall 来统计这部分访存推测相关的开销。

#### 2.5 缓存负责部分
各级缓存接收来自core的地址信号，与所有的Pending miss进行对比，如果匹配到了，并且确认是访存miss，那么用飞线发一个miss信号给core，否则发一个not found 信号。

只根据miss来判断，是因为正常的MSHR只保留miss的状态，未来我们的MSHR应该会正常化。

## 3 访存并行度计数器
对每一层缓存，对每一个MSHR，检查是否发生miss，并统计来源。至少需要区分来自CPU.data、预取器两个来源的并发miss 数量。因此需要添加如下的矩阵中的计数器：

|  | L1D 并发miss 数 | L2并发miss 数 | L3并发miss 数 |
| --- | --- | --- | --- |
| Cpu.data | L1DMLP_CPUData |  |  |
| 预取器 | L1DMLP_ Prefetcher | L2MLP_Prefetcher |  |
| Total |  |  |  |


矩阵中的每一项都是一个直方图histogram[32]，描述了并发miss数量为0-32的发生次数，统计方法：

For each cycle:

histogram[parallel miss count]++

访存并行度的统计数据不需要传递给core

