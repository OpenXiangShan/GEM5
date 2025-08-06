# 调度器相关代码介绍

## 1. 架构概述与演进历程

### 1.1 从集中式到分布式的演进

XS-GEM5的调度器架构体现了现代超标量处理器设计的重要演进：从传统的**集中式指令队列(Monolithic IQ)**向**分布式发射队列(Distributed IssueQueue)**架构的转变。这一转变不仅降低了硬件复杂性和功耗，还为提高时钟频率创造了条件。

#### 传统集中式架构的限制
```cpp
// 传统设计：单一庞大的指令队列
class MonolithicInstructionQueue {
    std::vector<DynInstPtr> entries[128];    // 大容量单一队列
    std::vector<std::vector<bool>> depMatrix; // NxN依赖矩阵，复杂度O(N²)
    ComplexSelector selector;                 // 复杂的全局选择逻辑
    // 问题：随着队列增大，硬件复杂度呈指数增长
};
```

#### 现代分布式架构的优势
```cpp
// 现代设计：多个小规模专用队列
class DistributedScheduler {
    std::vector<IssueQue*> issueQues;        // 多个小队列
    // 每个队列: 复杂度O(N/k)，总复杂度降为O(N)
    WakeupNetwork specWakeupChannels;         // 灵活的唤醒网络
    GlobalArbitrator portArbitrator;          // 全局资源仲裁
};
```

### 1.2 XS-GEM5调度器主要组件

- **Scheduler**: 中央协调者，负责指令分发、全局仲裁和唤醒协调
- **IssueQue**: 分布式发射队列，每个专门处理特定类型指令
- **InstructionQueue**: 兼容性包装器，提供对旧接口的支持
- **SpecWakeupChannel**: 推测性唤醒通道，实现跨队列性能优化

### 1.3 指令生命周期流程
```
Rename Stage → Dispatch Queues → Scheduler Distribution
       |              |                    |
       V              V                    V
   指令重命名 → 两阶段分发缓冲 → 分布式队列分配
       |              |                    |
       V              V                    V
   依赖分析   →   类型分类   →   局部依赖图构建
       |              |                    |
       V              V                    V
┌─────────────────────────────────────────────────┐
│              分布式发射逻辑                        │
│   ┌─────────┐ ┌─────────┐ ┌─────────┐           │
│   │ IntIQ0  │ │ FpIQ0   │ │ MemIQ0  │ ...       │
│   │ Ready   │ │ Ready   │ │ Ready   │           │
│   │ Select  │ │ Select  │ │ Select  │           │
│   └─────────┘ └─────────┘ └─────────┘           │
│            ↓        ↓        ↓                  │
│         Global Port Arbitration                │
│                    ↓                           │
│            Issue to Function Units             │
└─────────────────────────────────────────────────┘
       |              |                    |
       V              V                    V
   功能单元执行 → 推测性唤醒网络 → 依赖链传播
       |              |                    |
       V              V                    V
   结果写回    →   正式唤醒    →   下一轮发射
```

## 2. 核心数据结构深度解析

### 2.1 Scheduler 类：中央协调器 (src/cpu/o3/issue_queue.hh:231)

Scheduler作为分布式调度器的大脑，其设计体现了现代处理器"分治思想"的精髓。

#### 2.1.1 核心成员变量详解
```cpp
class Scheduler : public SimObject {
    // === 发射队列管理 ===
    std::vector<IssueQue*> issueQues;              // 管理的所有发射队列
    std::vector<DispPolicy> dispTable;             // OpClass→IssueQue映射表
    std::vector<int> dispSeqVec;                    // 分发序列缓存

    // === 三级记分板系统 ===
    std::vector<bool> scoreboard;                  // 正式记分板：真实数据就绪
    std::vector<bool> bypassScoreboard;            // 旁路记分板：旁路数据就绪  
    std::vector<bool> earlyScoreboard;             // 早期记分板：推测数据就绪

    // === 推测唤醒网络 ===
    std::vector<std::vector<IssueQue*>> wakeMatrix; // 唤醒矩阵[srcIQ][dstIQs]
    PendingWakeEventsType specWakeEvents;          // 待处理的推测唤醒事件
    
    // === 寄存器文件端口仲裁 ===
    std::vector<std::pair<DynInstPtr, int>> rfPortOccupancy; // 端口占用状态
    std::vector<DynInstPtr> arbFailedInsts;        // 仲裁失败的指令列表
    int rfMaxTypePortId;                           // 最大端口类型ID

    // === 执行控制 ===
    std::vector<DynInstPtr> instsToFu;             // 发射到功能单元的指令
    std::vector<bool> opPipelined;                 // 操作是否流水化
    std::vector<int> opExecTimeTable;              // 操作执行延迟表

    // === 性能统计 ===
    SchedulerStats stats;                          // 调度器性能统计
};
```

#### 2.1.2 三级记分板系统的设计哲学

**TODO: 似乎有问题，没有在代码中找到对应函数！**

```cpp
/*
 * 三级记分板系统实现了精确的数据依赖跟踪：
 * 
 * Level 1 - Early Scoreboard (推测就绪)：
 *   当指令发射时立即设置，用于激进的推测唤醒
 *   风险：可能被取消，需要回滚机制
 * 
 * Level 2 - Bypass Scoreboard (旁路就绪)：
 *   当指令开始执行、结果可通过旁路网络获取时设置
 *   用于支持back-to-back指令执行
 * 
 * Level 3 - Regular Scoreboard (正式就绪)：
 *   当指令完全执行完毕、结果写入寄存器文件时设置
 *   最保守但最可靠的就绪状态
 */

void Scheduler::updateScoreboard(const DynInstPtr& inst, ScoreboardLevel level) {
    for (int i = 0; i < inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) continue;
        
        switch (level) {
        case EARLY_READY:
            earlyScoreboard[dst->flatIndex()] = true;
            DPRINTF(Schedule, "[sn:%lli] Early ready: p%d\n", 
                    inst->seqNum, dst->flatIndex());
            break;
        case BYPASS_READY:
            bypassScoreboard[dst->flatIndex()] = true;
            DPRINTF(Schedule, "[sn:%lli] Bypass ready: p%d\n", 
                    inst->seqNum, dst->flatIndex());
            break;
        case FINAL_READY:
            scoreboard[dst->flatIndex()] = true;
            DPRINTF(Schedule, "[sn:%lli] Final ready: p%d\n", 
                    inst->seqNum, dst->flatIndex());
            break;
        }
    }
}
```

### 2.2 IssueQue 类：分布式微调度器 (src/cpu/o3/issue_queue.hh:102)

每个IssueQue实际上是一个完整的小型调度器，具备独立的指令管理、依赖跟踪和选择逻辑能力。

#### 2.2.1 指令存储与管理架构
```cpp
class IssueQue : public SimObject {
    // === 指令存储层次 ===
    std::list<DynInstPtr> instList;                    // 主存储：所有指令
    std::vector<ReadyQue*> readyQs;                     // 就绪队列：按输出端口分组
    SelectQue selectQ;                                  // 选择队列：仲裁后的候选
    std::queue<DynInstPtr> replayQ;                     // 重放队列：需重试的指令
    
    // === 多级发射流水线 ===
    TimeBuffer<IssueStream> inflightIssues;            // 发射流水线缓冲
    TimeBuffer<IssueStream>::wire toIssue;              // S0: 选择输出
    TimeBuffer<IssueStream>::wire toFu;                 // Sn: 发射到FU
    int scheduleToExecDelay;                            // 发射流水线深度
    
    // === 局部依赖图 ===
    // subDepGraph[physRegId] = [(srcOpIdx, dependentInst), ...]
    std::vector<std::vector<std::pair<uint8_t, DynInstPtr>>> subDepGraph;
    
    // === 端口配置与管理 ===
    std::vector<IssuePort*> oports;                     // 输出端口配置
    std::vector<int64_t> portBusy;                      // 端口占用状态
    int inports;                                        // 输入端口数量
    int outports;                                       // 输出端口数量
    
    // === 指令选择策略 ===
    BaseSelector* selector;                             // 选择器策略对象
    
    // === 容量管理 ===
    const int iqsize;                                   // 队列最大容量
    uint64_t instNum;                                   // 当前指令数
    std::vector<uint8_t> opNum;                         // 按OpClass的指令计数
};
```

#### 2.2.2 依赖图的精巧实现

**TODO: 有问题，没有在代码中找到对应函数！**

```cpp
/*
 * 局部依赖图设计：
 * - 索引方式：按物理寄存器索引 subDepGraph[physRegId]
 * - 存储内容：依赖该寄存器的所有指令及其源操作数位置
 * - 优势：O(1)查找依赖者，高效的唤醒传播
 */

void IssueQue::buildDependencyGraph(const DynInstPtr& inst) {
    DPRINTF(Schedule, "[sn:%lli] Building dependency graph\n", inst->seqNum);
    
    bool hasUnresolvedDeps = false;
    
    // 遍历所有源操作数
    for (int srcIdx = 0; srcIdx < inst->numSrcRegs(); srcIdx++) {
        auto srcReg = inst->renamedSrcIdx(srcIdx);
        
        if (srcReg->isFixedMapping()) {
            // 架构寄存器，总是就绪
            inst->markSrcRegReady(srcIdx);
            continue;
        }
        
        // 检查三级记分板状态
        if (scheduler->scoreboard[srcReg->flatIndex()]) {
            // 数据已在寄存器文件中
            inst->markSrcRegReady(srcIdx);
        } else if (scheduler->earlyScoreboard[srcReg->flatIndex()]) {
            // 可能通过推测获得数据
            inst->markSrcRegReady(srcIdx);
        } else {
            // 需要等待，建立依赖关系
            subDepGraph[srcReg->flatIndex()].emplace_back(srcIdx, inst);
            hasUnresolvedDeps = true;
            
            DPRINTF(Schedule, "[sn:%lli] Depends on p%d (src%d)\n", 
                    inst->seqNum, srcReg->flatIndex(), srcIdx);
        }
    }
    
    // 如果所有依赖都解决，加入就绪队列
    if (!hasUnresolvedDeps) {
        addToReadyQueue(inst);
    }
}
```

### 2.3 InstructionQueue 类：兼容性适配器 (src/cpu/o3/inst_queue.hh:106)

InstructionQueue现在主要作为向后兼容的适配器，将旧的集中式接口转换为新的分布式调度器调用。

#### 2.3.1 适配器模式的实现
```cpp
class InstructionQueue {
    // === 核心组件委托 ===
    Scheduler* scheduler;                               // 委托给分布式调度器
    MemDepUnit memDepUnit[MaxThreads];                  // 内存依赖单元
    
    // === 兼容性接口维护 ===
    std::list<DynInstPtr> instsToExecute;               // 执行就绪指令队列
    std::list<DynInstPtr> deferredMemInsts;             // 延迟的内存指令
    std::unordered_set<DynInstPtr> cacheMissLdInsts;    // 缓存缺失的Load
    std::list<STLFFailLdInst> stlfFailLdInsts;          // Store-to-Load转发失败
    std::list<DynInstPtr> blockedMemInsts;              // 被阻塞的内存指令
    std::list<DynInstPtr> retryMemInsts;                // 重试内存指令
    
    // === 非推测指令管理 ===
    std::map<InstSeqNum, DynInstPtr> nonSpecInsts;      // 非推测指令映射
    
    // === 统计信息 ===
    IQStats iqStats;                                    // 指令队列统计
    IQIOStats iqIOStats;                                // IO访问统计
    
public:
    // === 主要适配接口 ===
    void insert(const DynInstPtr &new_inst, int disp_seq) {
        scheduler->insert(new_inst, disp_seq);          // 委托给Scheduler
        ++iqStats.instsAdded;
    }
    
    bool hasReadyInsts() {
        return scheduler->hasReadyInsts();               // 委托查询
    }
    
    DynInstPtr getInstToExecute() {
        assert(!instsToExecute.empty());
        DynInstPtr inst = std::move(instsToExecute.front());
        instsToExecute.pop_front();
        return inst;
    }
    
    int wakeDependents(const DynInstPtr &completed_inst) {
        // 处理物理寄存器的释放逻辑（兼容性需要）
        completed_inst->lastWakeDependents = curTick();
        
        // 实际唤醒由Scheduler处理
        return 0;  // 分布式架构中，依赖计数不在此维护
    }
};
```

### 2.4 推测唤醒网络：性能加速的核心 (src/cpu/o3/issue_queue.hh:220)

推测唤醒是现代超标量处理器的关键性能优化技术，通过预测指令完成时间实现提前唤醒。

#### 2.4.1 唤醒通道的定义与配置
```cpp
class SpecWakeupChannel : public SimObject {
public:
    std::vector<std::string> srcIQs;                    // 源发射队列列表
    std::vector<std::string> dstIQs;                    // 目标发射队列列表
    
    SpecWakeupChannel(const SpecWakeupChannelParams& params)
        : SimObject(params), srcIQs(params.srcs), dstIQs(params.dsts) {}
};

/*
 * 香山V3处理器的典型唤醒网络配置：
 */
class KMHV3Scheduler : public Scheduler {
    // 定义各功能域的队列
    std::vector<std::string> intBank = {"intIQ0", "intIQ1", "intIQ2", "intIQ3", "intIQ4", "intIQ5"};
    std::vector<std::string> memBank = {"ld0", "ld1", "ld2", "sta0", "sta1", "std0", "std1"};
    std::vector<std::string> fpBank = {"fpIQ0", "fpIQ1", "fpIQ2", "fpIQ3"};
    
    // 配置唤醒网络
    std::vector<SpecWakeupChannel*> specWakeupNetwork = {
        // 整数域内部唤醒 + 整数到内存域唤醒
        new SpecWakeupChannel({.srcs = intBank + memBank, .dsts = intBank + memBank}),
        
        // 浮点域内部唤醒（隔离设计避免跨域干扰）
        new SpecWakeupChannel({.srcs = fpBank, .dsts = fpBank})
    };
};
```

对应scheduler 中wakeMatrix的构建

#### 2.4.2 推测唤醒事件的生命周期管理
```cpp
class SpecWakeupCompletion : public Event {
    DynInstPtr inst;                                    // 触发唤醒的指令
    IssueQue* to_issue_queue;                          // 目标发射队列
    PendingWakeEventsType* owner;                       // 事件管理器引用
    
public:
    SpecWakeupCompletion(const DynInstPtr& _inst, IssueQue* to, 
                        PendingWakeEventsType* _owner)
        : Event(Stat_Event_Pri, AutoDelete), inst(_inst), 
          to_issue_queue(to), owner(_owner) {}
    
    void process() override {
        // 执行推测唤醒
        to_issue_queue->wakeUpDependents(inst, true);
        
        // 清理事件记录
        (*owner)[inst->seqNum].erase(this);
        if ((*owner)[inst->seqNum].empty()) {
            owner->erase(inst->seqNum);
        }
        
        DPRINTF(Schedule, "[sn:%lli] Speculative wakeup completed\n", 
                inst->seqNum);
    }
    
    const char* description() const override {
        return "Speculative wakeup completion";
    }
};

/*
 * 推测唤醒的风险管理：
 * 如果推测失败（指令被squash），需要取消所有相关的推测唤醒事件
 */
void Scheduler::cancelSpeculativeWakeups(const DynInstPtr& inst) {
    auto it = specWakeEvents.find(inst->seqNum);
    if (it != specWakeEvents.end()) {
        for (auto* event : it->second) {
            cpu->deschedule(event);  // 取消事件调度
            delete event;            // 释放事件对象
        }
        specWakeEvents.erase(it);
        
        DPRINTF(Schedule, "[sn:%lli] Cancelled %d speculative wakeup events\n", 
                inst->seqNum, it->second.size());
    }
}
```
**TODO: 没有在代码中找到对应函数！cancelSpeculativeWakeups**

## 3. 分布式调度核心流程深度解析

### 3.1 两阶段指令分发机制

XS-GEM5实现了创新的两阶段分发机制，解耦了指令接收和队列分配，提高了流水线弹性。

#### 3.1.1 阶段一：分类到分发队列 (IEW::classifyInstToDispQue)
```cpp
/*
 * 第一阶段：预处理和缓冲
 * 目标：将来自Rename的指令按类型分类到缓冲队列
 * 优势：提供缓冲层，避免下游阻塞影响上游
 */
void IEW::classifyInstToDispQue(ThreadID tid) {
    std::deque<DynInstPtr> &insts_to_dispatch = 
        dispatchStatus[tid] == Unblocking ? skidBuffer[tid] : insts[tid];
    
    bool emptyROB = fromCommit->commitInfo[tid].emptyROB;
    unsigned dispatched = 0;
    
    while (!insts_to_dispatch.empty()) {
        auto& inst = insts_to_dispatch.front();
        
        // 1. 指令类型分类
        DQType dqType = getInstDQType(inst);  // IntDQ/FVDQ/MemDQ
        
        // 2. 容量检查
        if (dispQue[dqType].size() >= dqSize[dqType]) {
            // 对应类型的分发队列已满，暂停分发
            DPRINTF(Dispatch, "DispQueue[%d] full, blocking dispatch\n", dqType);
            break;
        }
        
        // 3. 序列化指令特殊处理
        if ((inst->isSerializeBefore() && !inst->isSerializeHandled()) && !emptyROB) {
            // 序列化指令需要等待ROB为空
            DPRINTF(Dispatch, "[sn:%lli] Serialize before, waiting for empty ROB\n", 
                    inst->seqNum);
            break;
        }
        
        // 4. 添加到分发队列
        dispQue[dqType].push_back(inst);
        inst->enterDQTick = curTick();  // 记录进入分发队列的时间
        
        // 5. 生产者注册（用于依赖图构建）
        if (!inst->isNop() && !inst->isEliminated()) {
            scheduler->addProducer(inst);  // 标记为数据生产者
        }
        
        insts_to_dispatch.pop_front();
        dispatched++;
    }
    
    // 更新分发统计
    iewStats.dispatchedInst[tid] += dispatched;
    
    // 处理阻塞情况
    if (!insts_to_dispatch.empty()) {
        block(tid);  // 阻塞当前线程的分发
    }
}

/*
 * 指令类型分类逻辑
 */
IEW::DQType IEW::getInstDQType(const DynInstPtr &inst) {
    // 内存相关指令进入MemDQ
    if (inst->isMemRef() || inst->isReadBarrier() || 
        inst->isWriteBarrier() || inst->isNonSpeculative()) {
        return MemDQ;
    }
    
    // 浮点和向量指令进入FVDQ
    if (inst->isFloating() || inst->isVector()) {
        return FVDQ;
    }
    
    // 其他指令进入IntDQ
    return IntDQ;
}
```

#### 3.1.2 阶段二：从分发队列到调度器 (IEW::dispatchInstFromDispQue)
```cpp
/*
 * 第二阶段：正式分发到调度器
 * 目标：将缓冲队列中的指令分发到具体的IssueQue
 * 特点：支持负载均衡和智能分发策略
 */
void IEW::dispatchInstFromDispQue(ThreadID tid) {
    bool add_to_iq = false;
    int totalDispatched = 0;
    
    // 遍历所有分发队列类型
    for (int dqType = 0; dqType < NumDQ; dqType++) {
        int dispatched = 0;
        int disp_seq = -1;
        
        // === 关键优化：预调度分析 ===
        // lookahead()会分析即将分发的指令，提前计算分发策略
        scheduler->lookahead(dispQue[dqType]);
        
        // 按分发带宽限制处理
        while (!dispQue[dqType].empty() && dispatched < dispWidth[dqType]) {
            DynInstPtr inst = dispQue[dqType].front();
            disp_seq++;  // 分发序列号，用于负载均衡
            
            // 跳过已废除指令
            if (inst->isSquashed()) {
                dispQue[dqType].pop_front();
                continue;
            }
            
            // === 关键检查：调度器就绪状态 ===
            if (!scheduler->ready(inst, disp_seq)) {
                // 目标IssueQue没有空间，停止当前类型的分发
                DPRINTF(Dispatch, "[sn:%lli] Scheduler not ready for %s\n", 
                        inst->seqNum, enums::OpClassStrings[inst->opClass()]);
                break;
            }
            
            // === LSQ容量检查（内存指令特有） ===
            if (checkLSQCapacity(inst, tid)) {
                DPRINTF(Dispatch, "[sn:%lli] LSQ capacity exhausted\n", inst->seqNum);
                break;
            }
            
            // === 指令分类处理 ===
            add_to_iq = classifyAndHandleInst(inst, tid);
            
            // === 非推测指令特殊处理 ===
            if (add_to_iq && inst->isNonSpeculative()) {
                inst->setCanCommit();
                instQueue.insertNonSpec(inst);  // 非推测指令专用队列
                add_to_iq = false;
            }
            
            // === 正式插入调度器 ===
            if (add_to_iq) {
                instQueue.insert(inst, disp_seq);  // 委托给调度器
            }
            
            // 清理和统计
            inst->exitDQTick = curTick();
            ppDispatch->notify(inst);  // 性能探针通知
            dispQue[dqType].pop_front();
            dispatched++;
            totalDispatched++;
        }
    }
    
    // 更新整体分发统计  
    iewStats.dispDist.sample(totalDispatched);
}
```

### 3.2 Scheduler智能分发策略

#### 3.2.1 预测性负载均衡 (Scheduler::lookahead)
```cpp
/*
 * lookahead机制：预测性分发策略
 * 目标：在实际分发前分析指令序列，优化队列分配
 * 核心思想：避免某个IssueQue成为瓶颈
 */
void Scheduler::lookahead(std::deque<DynInstPtr>& insts) {
    if (old_disp) {
        return;  // 传统分发模式，不使用预测
    }
    
    // 按OpClass统计即将分发的指令数量
    uint8_t disp_op_num[Num_OpClasses];
    std::memset(disp_op_num, 0, sizeof(disp_op_num));
    
    int i = 0;
    for (auto& inst : insts) {
        OpClass opClass = inst->opClass();
        
        // 获取该OpClass对应的IssueQue列表
        auto& iqs = dispTable[opClass];
        
        // === 关键策略：按负载排序 ===
        // 根据当前队列中该OpClass的指令数量排序，负载小的优先
        // 由于iqs 是引用，这里排序后已经修改了dispTable中的顺序！之后ready 就会按负载顺序放进去！
        std::sort(iqs.begin(), iqs.end(), disp_policy(opClass));
        
        // === Split Store特殊处理 ===
        if (inst->isSplitStoreAddr()) {
            // Store指令会被拆分为地址和数据两部分
            auto& stdIqs = dispTable[StoreDataOp];
            std::sort(stdIqs.begin(), stdIqs.end(), disp_policy(StoreDataOp));
        }
        
        // 计算该指令应该分发到哪个IssueQue（轮询策略）
        dispSeqVec[i] = disp_op_num[opClass] % iqs.size();
        disp_op_num[opClass]++;
        i++;
        
        DPRINTF(Schedule, "[sn:%lli] %s will go to IQ[%d] (load balancing)\n", 
                inst->seqNum, enums::OpClassStrings[opClass], dispSeqVec[i-1]);
    }
}

/*
 * 分发策略比较器：负载均衡核心算法
 */
bool Scheduler::disp_policy::operator()(IssueQue* a, IssueQue* b) const {
    // 比较两个IssueQue中指定OpClass的指令数量
    int loadA = a->opNum[disp_op];  // 队列A中该OpClass的指令数
    int loadB = b->opNum[disp_op];  // 队列B中该OpClass的指令数
    
    // 负载小的队列优先级更高
    return loadA < loadB;
}
```

#### 3.2.2 智能分发决策 (Scheduler::insert)
```cpp
/*
 * 主分发接口：将指令插入到最优的IssueQue
 */
void Scheduler::insert(const DynInstPtr& inst, int disp_seq) {
    DPRINTF(Schedule, "[sn:%lli] Inserting %s instruction\n", 
            inst->seqNum, enums::OpClassStrings[inst->opClass()]);
    
    // === Split Store指令特殊处理 ===
    if (inst->isSplitStoreAddr()) {
        // 1. 创建Store数据微操作
        auto stduop = inst->createStoreDataUop();
        this->insert(stduop, disp_seq);  // 递归插入数据部分
        
        // 2. 将原指令转换为Store地址微操作
        inst->buildStoreAddrUop();
        DPRINTF(Schedule, "[sn:%lli] Split store: addr=%lli, data=%lli\n", 
                inst->seqNum, inst->seqNum, stduop->seqNum);
    }
    
    // 获取目标IssueQue列表
    auto& iqs = dispTable[inst->opClass()];
    assert(!iqs.empty());
    
    if (old_disp) {
        // === 传统分发：遍历查找可用队列 ===
        bool inserted = false;
        std::sort(iqs.begin(), iqs.end(), disp_policy(inst->opClass()));
        
        for (auto iq : iqs) { // 遍历所有IssueQue，找到一个ready的IssueQue，将指令插入进去，RTL开销大
            if (iq->ready()) {
                iq->insert(inst);
                inserted = true;
                break;
            }
        }
        panic_if(!inserted, "No available IQ for opClass %s", 
                 enums::OpClassStrings[inst->opClass()]);
    } else {
        // === 现代分发：基于预测的精确分配 ===
        IssueQue* targetIQ = iqs[dispSeqVec.at(disp_seq)]; // 按照lookahead 的顺序，将指令插入到对应的IssueQue中
        assert(targetIQ->ready());
        targetIQ->insert(inst);
        
        DPRINTF(Schedule, "[sn:%lli] Assigned to %s (seq=%d)\n", 
                inst->seqNum, targetIQ->getName().c_str(), disp_seq);
    }
}
```

### 3.3 分布式发射与选择机制

#### 3.3.1 "局部选择，全局仲裁"架构
```cpp
/*
 * 核心调度函数：协调所有IssueQue的发射过程
 * 实现"局部选择，全局仲裁"的分布式调度策略
 */
void Scheduler::issueAndSelect() {
    DPRINTF(Schedule, "=== Starting distributed issue and select cycle ===\n");
    
    // === 第一步：局部选择阶段 ===
    // 每个IssueQue独立选择其最佳候选指令
    for (auto iq : issueQues) {
        iq->selectInst();  // 局部选择，结果存储在iq->selectQ中
    }
    
    // === 第二步：全局仲裁阶段 ===
    // 处理跨队列的资源冲突（主要是寄存器文件端口冲突）
    globalPortArbitration();
    
    // === 第三步：正式发射阶段 ===  
    // 通过仲裁的指令正式发射到功能单元
    for (auto iq : issueQues) {
        iq->issueToFu();  // 将selectQ中的获胜指令发射出去
    }
    
    // === 第四步：性能统计更新 ===
    updateIssueStats();
}

/*
 * 全局寄存器文件端口仲裁
 * 解决多个队列同时请求相同读端口的冲突
 */
void Scheduler::globalPortArbitration() {
    // 清空仲裁失败列表
    arbFailedInsts.clear();
    
    // 重置端口占用状态
    std::fill(rfPortOccupancy.begin(), rfPortOccupancy.end(), 
              std::make_pair(nullptr, 0));
    
    // 收集所有候选指令的端口需求
    for (auto iq : issueQues) {
        for (auto& [portId, inst] : iq->selectQ) {
            // 检查每个源操作数的端口需求
            for (int srcIdx = 0; srcIdx < inst->numSrcRegs(); srcIdx++) {
                auto srcReg = inst->renamedSrcIdx(srcIdx);
                if (srcReg->isFixedMapping()) continue;
                
                // 获取端口配置
                int typePortId = getRegFilePortId(iq, portId, srcIdx, srcReg);
                int priority = getPortPriority(iq, portId, srcIdx);
                
                // 尝试分配端口
                if (!allocateRegFilePort(inst, srcReg, typePortId, priority)) {
                    // 仲裁失败，将指令加入失败列表
                    arbFailedInsts.push_back(inst);
                    DPRINTF(Schedule, "[sn:%lli] Port arbitration failed for p%d\n", 
                            inst->seqNum, srcReg->flatIndex());
                    break;  // 该指令失败，检查下一条指令
                }
            }
        }
    }
    
    // 通知各IssueQue仲裁结果
    for (auto& inst : arbFailedInsts) {
        inst->setArbFailed();  // 标记仲裁失败，下周期重试
    }
    
    DPRINTF(Schedule, "Port arbitration: %d instructions failed\n", 
            arbFailedInsts.size());
}
```

#### 3.3.2 IssueQue局部选择逻辑
```cpp
/*
 * 单个IssueQue的指令选择过程
 * 目标：从就绪队列中选出最优候选指令
 */
void IssueQue::selectInst() {
    selectQ.clear();  // 清空上一周期的选择结果
    
    // 遍历所有输出端口
    for (int portId = 0; portId < outports; portId++) {
        auto readyQ = readyQs[portId];
        if (readyQ->empty()) continue;
        
        // 使用选择器策略选择最佳指令
        selector->begin(readyQ);  // 设置选择范围
        
        // 按照选择器策略，逐个选择最佳指令
        for (auto it = selector->select(readyQ->begin(), portId); 
             it != readyQ->end(); 
             it = selector->select(++it, portId)) {
            
            auto& inst = *it;
            
            // 跳过已取消的指令
            if (inst->canceled()) {
                inst->clearInReadyQ();
                it = readyQ->erase(it);
                continue;
            }
            
            // === 关键检查：端口可用性 ===
            // 检查该端口在未来几个周期是否被占用
            uint64_t futureBusyMask = portBusy[portId];
            int instOpLatency = scheduler->getCorrectedOpLat(inst);
            
            if (!(futureBusyMask & (1llu << instOpLatency))) {
                // 端口可用，选择该指令
                DPRINTF(Schedule, "[sn:%lli] Selected for port %d\n", 
                        inst->seqNum, portId);
                
                // 预分配寄存器文件读端口
                bool portAllocSuccess = preAllocateRegFilePorts(inst, portId);
                if (portAllocSuccess) {
                    selectQ.push_back(std::make_pair(portId, inst));
                    inst->clearInReadyQ();
                    readyQ->erase(it);
                    break;  // 该端口已选定指令，选择下一个端口
                } else {
                    // 端口预分配失败，尝试下一条指令
                    continue;
                }
            } else {
                // 端口忙，记录统计信息
                iqstats->portBusy[portId]++;
                DPRINTF(Schedule, "[sn:%lli] Port %d busy, mask=0x%llx\n", 
                        inst->seqNum, portId, futureBusyMask);
            }
        }
    }
    
    DPRINTF(Schedule, "%s selected %d instructions this cycle\n", 
            iqname.c_str(), selectQ.size());
}

/*
 * 寄存器文件端口预分配
 * 在全局仲裁前，先进行本地端口需求分析
 */
bool IssueQue::preAllocateRegFilePorts(const DynInstPtr& inst, int portId) {
    // 获取该端口的寄存器文件配置
    auto& intPorts = intRfTypePortId[portId];
    auto& fpPorts = fpRfTypePortId[portId];
    
    // 检查每个源操作数
    for (int srcIdx = 0; srcIdx < inst->numSrcRegs(); srcIdx++) {
        auto srcReg = inst->srcRegIdx(srcIdx);
        auto physReg = inst->renamedSrcIdx(srcIdx);
        
        if (physReg->isFixedMapping()) continue;
        
        std::pair<int, int> typePortIdPri;
        bool portFound = false;
        
        // 根据寄存器类型选择端口
        if (srcReg.isIntReg() && srcIdx < intPorts.size()) {
            typePortIdPri = intPorts[srcIdx];
            portFound = true;
        } else if (srcReg.isFloatReg() && srcIdx < fpPorts.size()) {
            typePortIdPri = fpPorts[srcIdx];
            portFound = true;
        }
        
        if (portFound) {
            // 通知调度器进行端口分配
            scheduler->useRegfilePort(inst, physReg, 
                                    typePortIdPri.first,   // typePortId
                                    typePortIdPri.second); // priority
        }
    }
    
    return true;  // 预分配总是成功，实际冲突由全局仲裁解决
}
```

### 3.4 多级唤醒机制深度解析

#### 3.4.1 推测性唤醒：性能优化的核心
```cpp
/*
 * 推测性唤醒：现代超标量处理器的关键技术
 * 原理：在指令发射时就预测其完成时间，提前唤醒依赖指令
 * 优势：减少依赖链延迟，提高指令级并行度
 * 风险：推测错误需要回滚机制
 */
void Scheduler::specWakeUpDependents(const DynInstPtr& inst, IssueQue* from_issue_queue) {
    // 只有流水化且有目标寄存器的非Load指令才进行推测唤醒
    if (!opPipelined[inst->opClass()] || inst->numDestRegs() == 0 || inst->isLoad()) {
        DPRINTF(Schedule, "[sn:%lli] Skip spec wakeup: not eligible\n", inst->seqNum);
        return;
    }
    
    DPRINTF(Schedule, "[sn:%lli] Starting speculative wakeup from %s\n", 
            inst->seqNum, from_issue_queue->getName().c_str());
    
    // 遍历推测唤醒网络，找到所有目标队列
    for (auto to : wakeMatrix[from_issue_queue->getId()]) {
        // 计算唤醒延迟
        int baseOpLatency = getCorrectedOpLat(inst);
        int wakeDelay = baseOpLatency - 1;  // 提前一个周期唤醒
        
        // === 跨队列延迟调整 ===
        // 不同深度的发射流水线需要调整唤醒时机
        int stagesDiff = std::abs(from_issue_queue->getIssueStages() - 
                                 to->getIssueStages());
        
        if (from_issue_queue->getIssueStages() > to->getIssueStages()) {
            wakeDelay += stagesDiff;  // 目标队列更浅，延迟增加
        } else if (wakeDelay >= stagesDiff) {
            wakeDelay -= stagesDiff;  // 目标队列更深，延迟减少
        }
        
        DPRINTF(Schedule, "[sn:%lli] Wakeup %s->%s, delay=%d cycles\n", 
                inst->seqNum, from_issue_queue->getName().c_str(), 
                to->getName().c_str(), wakeDelay);
        
        if (wakeDelay == 0) {
            // === 立即推测唤醒 ===
            to->wakeUpDependents(inst, true);  // 推测性标志为true
            
            // 更新早期记分板（仅限整数指令）
            if (!(inst->isFloating() || inst->isVector())) {
                for (int i = 0; i < inst->numDestRegs(); i++) {
                    auto dst = inst->renamedDestIdx(i);
                    if (!dst->isFixedMapping()) {
                        earlyScoreboard[dst->flatIndex()] = true;
                        DPRINTF(Schedule, "[sn:%lli] Early scoreboard set: p%d\n", 
                                inst->seqNum, dst->flatIndex());
                    }
                }
            }
        } else {
            // === 延迟推测唤醒 ===
            // 创建推测唤醒事件
            auto wakeEvent = new SpecWakeupCompletion(inst, to, &specWakeEvents);
            
            // 记录事件用于可能的取消操作
            specWakeEvents[inst->seqNum].insert(wakeEvent);
            
            // 调度事件
            Tick whenToWake = cpu->clockEdge(Cycles(wakeDelay)) - 1;
            cpu->schedule(wakeEvent, whenToWake);
            
            DPRINTF(Schedule, "[sn:%lli] Scheduled spec wakeup event at tick %llu\n", 
                    inst->seqNum, whenToWake);
        }
    }
}

/*
 * Load指令专用的推测唤醒
 * Load的特殊性：延迟不确定，但可以在流水线中提前唤醒依赖者
 */
void Scheduler::specWakeUpFromLoadPipe(const DynInstPtr& inst) {
    assert(inst->isLoad());
    
    auto from_issue_queue = inst->issueQue;
    DPRINTF(Schedule, "[sn:%lli] Load pipe spec wakeup from %s\n", 
            inst->seqNum, from_issue_queue->getName().c_str());
    
    // Load指令的推测唤醒总是立即进行（在Load流水线中）
    for (auto to : wakeMatrix[from_issue_queue->getId()]) {
        to->wakeUpDependents(inst, true);
        
        // 设置早期记分板
        for (int i = 0; i < inst->numDestRegs(); i++) {
            auto dst = inst->renamedDestIdx(i);
            if (!dst->isFixedMapping()) {
                earlyScoreboard[dst->flatIndex()] = true;
            }
        }
    }
}
```

#### 3.4.2 正式唤醒：可靠的依赖解除
```cpp
/*
 * 写回阶段的正式唤醒
 * 特点：数据已经真实可用，唤醒是安全和可靠的
 */
void Scheduler::writebackWakeup(const DynInstPtr& inst) {
    DPRINTF(Schedule, "[sn:%lli] Writeback wakeup started\n", inst->seqNum);
    
    inst->setWriteback();  // 标记指令已写回
    cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtWriteVal);
    
    // 更新正式记分板
    for (int i = 0; i < inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (!dst->isFixedMapping()) {
            scoreboard[dst->flatIndex()] = true;
            DPRINTF(Schedule, "[sn:%lli] Scoreboard set: p%d\n", 
                    inst->seqNum, dst->flatIndex());
        }
    }
    
    // 通知所有IssueQue进行正式唤醒
    for (auto iq : issueQues) {
        iq->wakeUpDependents(inst, false);  // 非推测性唤醒
    }
}

/*
 * IssueQue内的依赖唤醒实现
 */
void IssueQue::wakeUpDependents(const DynInstPtr& inst, bool speculative) {
    if (speculative && inst->canceled()) {
        // 推测性唤醒时，如果指令已被取消，则跳过
        return;
    }
    
    DPRINTF(Schedule, "[sn:%lli] %s wakeup in %s\n", 
            inst->seqNum, speculative ? "Speculative" : "Writeback", iqname.c_str());
    
    // 遍历该指令的所有目标寄存器
    for (int i = 0; i < inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping() || dst->getNumPinnedWritesToComplete() != 1) {
            continue;  // 跳过架构寄存器和多写端口寄存器
        }
        
        // 寄存器缓存更新（整数寄存器优化）
        if (dst->is(IntRegClass)) {
            scheduler->regCache.insert(dst->flatIndex(), {});
        }
        
        // 查找依赖该寄存器的所有指令
        auto& depList = subDepGraph[dst->flatIndex()];
        for (auto it = depList.begin(); it != depList.end(); ) {
            int srcIdx = it->first;
            auto& consumer = it->second;
            
            // 检查依赖指令是否已被废弃
            if (consumer->isSquashed()) {
                it = depList.erase(it);
                continue;
            }
            
            // 检查该源操作数是否已经就绪
            if (consumer->readySrcIdx(srcIdx)) {
                ++it;  // 已经就绪，跳过
                continue;
            }
            
            // === 关键步骤：标记源操作数就绪 ===
            consumer->markSrcRegReady(srcIdx);
            DPRINTF(Schedule, "[sn:%lli] src%d woken by [sn:%lli]\n", 
                    consumer->seqNum, srcIdx, inst->seqNum);
            
            // 检查指令是否所有依赖都已解决
            addIfReady(consumer);
            
            ++it;
        }
        
        // 正式唤醒时清理依赖图
        if (!speculative) {
            depList.clear();
        }
    }
}

/*
 * 检查指令是否就绪并加入就绪队列
 */
void IssueQue::addIfReady(const DynInstPtr& inst) {
    if (!inst->readyToIssue()) {
        return;  // 还有未解决的依赖
    }
    
    // 记录就绪时间戳
    if (inst->readyTick == -1) {
        inst->readyTick = curTick();
        DPRINTF(Counters, "[sn:%lli] Ready at tick %llu\n", 
                inst->seqNum, curTick());
    }
    
    // 内存指令需要额外检查内存依赖
    if (inst->isMemRef() && !inst->memDepSolved()) {
        DPRINTF(Schedule, "[sn:%lli] Memory dependency not solved\n", inst->seqNum);
        return;
    }
    
    // 清除取消标志并加入就绪队列
    inst->clearCancel();
    if (!inst->inReadyQ()) {
        READYQ_PUSH(inst);  // 宏定义的就绪队列插入操作
        DPRINTF(Schedule, "[sn:%lli] Added to ready queue\n", inst->seqNum);
    }
}
```

## 4. Dispatch与Execute深度交互机制

### 4.1 调度器与执行单元的精密协调

#### 4.1.1 指令发射到功能单元的完整流程
```cpp
/*
 * 从调度器到功能单元的完整路径
 * 体现了现代处理器精确的资源管理和时序控制
 */
DynInstPtr Scheduler::getInstToFU() {
    if (instsToFu.empty()) {
        return DynInstPtr(nullptr);  // 没有待发射指令
    }
    
    // 从发射队列取出指令
    auto inst = instsToFu.back();
    instsToFu.pop_back();
    
    DPRINTF(Schedule, "[sn:%lli] Issued to FU: %s\n", 
            inst->seqNum, enums::OpClassStrings[inst->opClass()]);
    
    // 更新指令状态和时间戳
    inst->setIssued();
    inst->issueTick = curTick();
    
    // 性能跟踪点
    cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtFU);
    
    return inst;
}

/*
 * 执行单元延迟管理：动态延迟预测
 * 某些指令的执行延迟取决于操作数值，需要动态计算
 */
uint32_t Scheduler::getOpLatency(const DynInstPtr& inst) {
    OpClass opClass = inst->opClass();
    
    // 浮点转换指令的特殊处理
    if (opClass == FloatCvtOp) {
        if (inst->destRegIdx(0).isFloatReg()) {
            // 整数到浮点转换需要额外延迟
            return 2 + opExecTimeTable[opClass];
        }
    }
    
    // 查表获取基础延迟
    uint32_t baseLatency = opExecTimeTable[opClass];
    
    DPRINTF(Schedule, "[sn:%lli] Op latency for %s: %d cycles\n", 
            inst->seqNum, enums::OpClassStrings[opClass], baseLatency);
    
    return baseLatency;
}

/*
 * 修正后的操作延迟：考虑流水线和旁路因素
 */
uint32_t Scheduler::getCorrectedOpLat(const DynInstPtr& inst) {
    uint32_t baseLatency = getOpLatency(inst);
    
    // 未来可以在这里添加更多修正因素：
    // - 操作数准备时间
    // - 旁路网络延迟
    // - 功能单元流水线深度
    
    return baseLatency;
}
```

#### 4.1.2 功能单元完成事件处理
```cpp
/*
 * 功能单元完成事件类：处理长延迟操作
 * 对于延迟超过1个周期的操作，使用事件驱动机制
 */
class InstructionQueue::FUCompletion : public Event {
    DynInstPtr inst;                    // 执行完成的指令 
    int fuIdx;                         // 功能单元索引
    InstructionQueue *iqPtr;           // 指令队列指针
    bool freeFU;                       // 是否释放功能单元

public:
    FUCompletion(const DynInstPtr &_inst, int fu_idx, InstructionQueue *iq_ptr)
        : Event(Stat_Event_Pri, AutoDelete), inst(_inst), fuIdx(fu_idx), 
          iqPtr(iq_ptr), freeFU(false) {}
    
    void process() override {
        DPRINTF(IEW, "[sn:%lli] FU completion event processed\n", inst->seqNum);
        
        // 处理功能单元完成
        iqPtr->processFUCompletion(inst, fuIdx);
        
        // 清理指令引用
        inst = nullptr;
    }
    
    const char* description() const override {
        return "Functional unit completion";
    }
    
    void setFreeFU() { freeFU = true; }
};

/*
 * 处理功能单元完成的核心逻辑
 */
void InstructionQueue::processFUCompletion(const DynInstPtr &inst, int fu_idx) {
    DPRINTF(IEW, "[sn:%lli] Processing FU completion\n", inst->seqNum);
    
    // 检查CPU是否处于睡眠状态
    assert(!cpu->switchedOut());
    
    // 减少未完成的写回操作计数
    --wbOutstanding;
    
    // 唤醒可能睡眠的CPU
    iewStage->wakeCPU();
    
    // 将指令添加到写回队列
    // 注：这些FU完成事件应该在周期开始时处理，避免时序问题
    issueToExecuteQueue->access(0)->size++;
    instsToExecute.push_back(inst);
    
    DPRINTF(IEW, "[sn:%lli] Added to execute queue, size=%d\n", 
            inst->seqNum, issueToExecuteQueue->access(0)->size);
}
```

### 4.2 执行延迟的精确建模

#### 4.2.1 动态延迟计算：真实硬件的精确建模
```cpp
/*
 * 执行延迟检查：基于操作数值的动态延迟预测
 * 体现了现代处理器对执行延迟的精确建模
 */
bool InstructionQueue::execLatencyCheck(const DynInstPtr& inst, uint32_t& op_latency) {
    // Leading zero count：用于除法延迟计算的辅助函数
    auto clz = [](RegVal val) -> int {
#if defined(__GNUC__) || defined(__clang__)
        return val == 0 ? 64 : __builtin_clzll(val);
#else
        // 软件实现的前导零计数
        for (int i = 0; i < 64; i++) {
            if (val & (0x1lu << 63)) return i;
            val <<= 1;
        }
        return 64;
#endif
    };

    RegVal rs1, rs2;
    int delay_;
    
    switch (inst->opClass()) {
    case OpClass::IntDiv: {
        // 整数除法：延迟取决于操作数的位宽差异
        rs1 = cpu->readArchIntReg(inst->srcRegIdx(0).index(), inst->threadNumber);
        rs2 = cpu->readArchIntReg(inst->srcRegIdx(1).index(), inst->threadNumber);
        
        // 计算前导零差异：rs1和rs2的有效位宽差异
        delay_ = std::max(clz(rs2) - clz(rs1), 0);
        
        if (rs2 == 1) {
            // 除以1的特殊情况：rs1 / 1 = rs1
            op_latency = 5;
        } else if (rs1 == rs2) {
            // 相等除法：rs1 / rs2 = 1 余 0
            op_latency = 7;
        } else if (clz(rs2) - clz(rs1) < 0) {
            // 被除数小于除数：rs1/rs2 = 0 余 rs1
            op_latency = 5;
        } else {
            // 一般情况：基础延迟 + 动态延迟
            op_latency = 7 + delay_ / 4;
        }
        
        DPRINTF(IEW, "[sn:%lli] IntDiv latency: %d (rs1=0x%llx, rs2=0x%llx)\n", 
                inst->seqNum, op_latency, rs1, rs2);
        return true;
    }
    
    case OpClass::FloatSqrt: {
        // 浮点平方根：延迟取决于精度和特殊值
        rs1 = cpu->readArchFloatReg(inst->srcRegIdx(0).index(), inst->threadNumber);
        rs2 = cpu->readArchFloatReg(inst->srcRegIdx(1).index(), inst->threadNumber);
        
        switch (inst->staticInst->operWid()) {
        case 32: {  // 单精度
            float* f1 = reinterpret_cast<float*>(&rs1);
            float* f2 = reinterpret_cast<float*>(&rs2);
            
            if (__isnanf(*f1) || __isnanf(*f2) || __isinff(*f1) || __isinff(*f2)) {
                // 特殊值（NaN, Inf）快速完成
                op_latency = 2;
            } else {
                // 正常单精度平方根
                op_latency = 8;
            }
            break;
        }
        case 64: {  // 双精度
            double* d1 = reinterpret_cast<double*>(&rs1);
            double* d2 = reinterpret_cast<double*>(&rs2);
            
            if (__isnan(*d1) || __isnan(*d2) || __isinf(*d1) || __isinf(*d2)) {
                // 特殊值快速完成
                op_latency = 2;
            } else {
                // 正常双精度平方根
                op_latency = 15;
            }
            break;
        }
        default:
            panic("Unsupported float width: %d\n", inst->staticInst->operWid());
            return false;
        }
        
        DPRINTF(IEW, "[sn:%lli] FloatSqrt latency: %d (%d-bit)\n", 
                inst->seqNum, op_latency, inst->staticInst->operWid());
        return true;
    }
    
    case OpClass::FloatDiv: {
        // 浮点除法：类似平方根的处理
        rs1 = cpu->readArchFloatReg(inst->srcRegIdx(0).index(), inst->threadNumber);
        rs2 = cpu->readArchFloatReg(inst->srcRegIdx(1).index(), inst->threadNumber);
        
        switch (inst->staticInst->operWid()) {
        case 32: {
            float* f1 = reinterpret_cast<float*>(&rs1);
            float* f2 = reinterpret_cast<float*>(&rs2);
            
            op_latency = (__isnanf(*f1) || __isnanf(*f2) || 
                         __isinff(*f1) || __isinff(*f2)) ? 2 : 7;
            break;
        }
        case 64: {
            double* d1 = reinterpret_cast<double*>(&rs1);
            double* d2 = reinterpret_cast<double*>(&rs2);
            
            op_latency = (__isnan(*d1) || __isnan(*d2) || 
                         __isinf(*d1) || __isinf(*d2)) ? 2 : 12;
            break;
        }
        default:
            panic("Unsupported float width: %d\n", inst->staticInst->operWid());
            return false;
        }
        
        DPRINTF(IEW, "[sn:%lli] FloatDiv latency: %d (%d-bit)\n", 
                inst->seqNum, op_latency, inst->staticInst->operWid());
        return true;
    }
    
    default:
        // 其他指令使用固定延迟
        return false;
    }
}
```

### 4.3 执行流水线的精确时序控制

#### 4.3.1 多级发射流水线管理
```cpp
/*
 * IssueQue的多级发射流水线实现
 * 支持不同深度的发射流水线，精确控制指令时序
 */
void IssueQue::scheduleInst() {
    DPRINTF(Schedule, "%s: Scheduling %d instructions\n", iqname.c_str(), selectQ.size());
    
    // 处理选择队列中的每条指令
    for (auto& [portId, inst] : selectQ) {
        if (inst->canceled()) {
            DPRINTF(Schedule, "[sn:%lli] Canceled during schedule\n", inst->seqNum);
            continue;
        }
        
        if (inst->arbFailed()) {
            // 仲裁失败，放回就绪队列重试
            DPRINTF(Schedule, "[sn:%lli] Arbitration failed, retry\n", inst->seqNum);
            iqstats->arbFailed++;
            
            assert(inst->readyToIssue());
            READYQ_PUSH(inst);  // 重新加入就绪队列
        } else {
            // 成功调度
            DPRINTF(Schedule, "[sn:%lli] Successfully scheduled to port %d\n", 
                    inst->seqNum, portId);
            
            iqstats->portissued[portId]++;
            inst->setScheduled();
            inst->issueportid = portId;
            
            // === 关键：加入发射流水线 ===
            toIssue->push(inst);  // 进入发射流水线的第一级
            
            // 端口占用管理
            if (!opPipelined[inst->opClass()]) {
                // 非流水化操作：完全占用端口
                portBusy[portId] = -1ll;  // 全部位设置为1
            } else if (scheduler->getCorrectedOpLat(inst) > 1) {
                // 流水化操作：按延迟设置占用位
                uint32_t latency = scheduler->getCorrectedOpLat(inst);
                portBusy[portId] |= (1ll << latency);
            }
            
            // === 启动推测唤醒 ===
            scheduler->specWakeUpDependents(inst, this);
            
            // 性能跟踪
            cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtIssueArb);
        }
        
        // 清理仲裁失败标志
        inst->clearArbFailed();
    }
}

/*
 * 发射流水线推进：从选择到功能单元的多级缓冲
 */
void IssueQue::tick() {
    // 更新统计信息
    iqstats->avgInsts = instNum;
    
    if (instNumInsert > 0) {
        iqstats->insertDist[instNumInsert]++;
        instNumInsert = 0;
    }
    
    // === 核心：调度当前周期选中的指令 ===
    scheduleInst();
    
    // === 推进发射流水线 ===
    inflightIssues.advance();  // 所有级都向前推进一级
    
    // === 更新端口占用状态 ===
    for (auto& busyMask : portBusy) {
        busyMask = busyMask >> 1;  // 所有占用位右移一位
    }
    
    DPRINTF(Schedule, "%s tick complete, %d insts in flight\n", 
            iqname.c_str(), getTotalInflightInsts());
}

/*
 * 从发射流水线末端发射到功能单元
 */
void IssueQue::issueToFu() {
    int size = toFu->size;  // 本周期可发射的指令数
    int replayed = 0;       // 重放指令计数
    int issued = 0;         // 总发射指令计数
    
    int issuedLoad = 0;     // 发射的Load指令数
    int issuedStore = 0;    // 发射的Store指令数
    
    // === 第一优先级：处理重放队列 ===
    // 重放指令通常是因为缓存缺失等原因需要重试的内存指令
    while (!replayQ.empty() && replayed < outports) {
        auto& inst = replayQ.front();
        
        // 检查Load/Store流水线容量限制
        if (inst->isLoad() && issuedLoad >= numLoadPipe) break;
        if (inst->isStore() && issuedStore >= numStorePipe) break;
        
        // 直接发射到功能单元，无需再次检查记分板
        scheduler->addToFU(inst);
        DPRINTF(Schedule, "[sn:%lli] Replayed to FU\n", inst->seqNum);
        
        replayQ.pop();
        issued++;
        replayed++;
        
        if (inst->isLoad()) issuedLoad++;
        if (inst->isStore()) issuedStore++;
    }
    
    // === 第二优先级：处理发射流水线输出 ===
    for (int i = 0; i < size; i++) {
        auto inst = toFu->pop();
        if (!inst) continue;
        
        // 检查发射端口和流水线容量
        if ((i + replayed >= outports) ||
            (inst->isLoad() && issuedLoad >= numLoadPipe) ||
            (inst->isStore() && issuedStore >= numStorePipe)) {
            
            // 容量不足，将指令放回就绪队列
            inst->clearScheduled();
            READYQ_PUSH(inst);
            
            DPRINTF(Schedule, "[sn:%lli] Issue port/pipe occupied, retry\n", inst->seqNum);
            iqstats->issueOccupy++;
            continue;
        }
        
        // === 关键检查：记分板状态验证 ===
        if (!checkScoreboard(inst)) {
            // 记分板检查失败（通常是推测唤醒错误）
            DPRINTF(Schedule, "[sn:%lli] Scoreboard check failed\n", inst->seqNum);
            continue;  // 指令被取消，不计入发射统计
        }
        
        // 成功发射
        if (inst->isLoad()) issuedLoad++;
        if (inst->isStore()) issuedStore++;
        
        addToFu(inst);  // 实际发射到功能单元
        cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtIssueReadReg);
        issued++;
    }
    
    // 更新统计信息
    if (issued > 0) {
        iqstats->issueDist[issued]++;
    }
    if (replayed > 0) {
        iqstats->issueOccupy += replayed;  // 记录重放开销
    }
    
    DPRINTF(Schedule, "%s issued %d insts (%d replayed) this cycle\n", 
            iqname.c_str(), issued, replayed);
}
```

### 4.4 记分板验证与推测恢复

#### 4.4.1 精确的记分板检查机制
```cpp
/*
 * 记分板检查：验证推测唤醒的正确性
 * 这是防止错误推测导致程序错误的关键机制
 */
bool IssueQue::checkScoreboard(const DynInstPtr& inst) {
    DPRINTF(Schedule, "[sn:%lli] Checking scoreboard\n", inst->seqNum);
    
    for (int i = 0; i < inst->numSrcRegs(); i++) {
        auto src = inst->renamedSrcIdx(i);
        
        if (src->isFixedMapping()) {
            // 架构寄存器总是可用
            continue;
        }
        
        // === 关键检查：旁路数据可用性 ===
        if (!scheduler->bypassScoreboard[src->flatIndex()]) {
            // 旁路数据不可用，需要找到生产者指令
            auto dst_inst = scheduler->getInstByDstReg(src->flatIndex());
            // 如果dst_inst 是null，或者不是load，则说明这个指令的源操作数没有被任何指令生产，panic
            if (!dst_inst || !dst_inst->isLoad()) {
                panic("Invalid dependency: expected load producer for p%d", 
                      src->flatIndex());
            }
            
            DPRINTF(Schedule, "[sn:%lli] Cannot get bypass data from [sn:%lli], canceling\n", 
                    inst->seqNum, dst_inst->seqNum);
            
            // === 推测错误处理：Load取消传播 ===
            scheduler->loadCancel(dst_inst);
            return false;  // 检查失败
        }
    }
    
    DPRINTF(Schedule, "[sn:%lli] Scoreboard check passed\n", inst->seqNum);
    return true;  // 所有源操作数都可用
}

/*
 * Load取消传播：处理推测错误的级联影响
 * 当Load指令缓存缺失时，需要取消所有推测唤醒的依赖指令
 */
void Scheduler::loadCancel(const DynInstPtr& inst) {
    DPRINTF(Schedule, "[sn:%lli] Load miss, starting cancel propagation\n", inst->seqNum);
    
    if (inst->issueQue) {
        inst->issueQue->iqstats->loadmiss++;
    }
    
    // 使用DFS遍历依赖链，取消所有受影响的指令
    std::stack<DynInstPtr> cancelStack;
    cancelStack.push(inst);
    
    while (!cancelStack.empty()) {
        auto cancelInst = cancelStack.top();
        cancelStack.pop();
        
        // === 清理推测唤醒事件 ===
        auto eventIt = specWakeEvents.find(cancelInst->seqNum);
        if (eventIt != specWakeEvents.end()) {
            for (auto* event : eventIt->second) {
                cpu->deschedule(event);  // 取消调度的事件
                delete event;            // 释放事件对象
            }
            specWakeEvents.erase(eventIt);
            
            DPRINTF(Schedule, "[sn:%lli] Cancelled %d spec wakeup events\n", 
                    cancelInst->seqNum, eventIt->second.size());
        }
        
        // === 更新记分板状态 ===
        for (int i = 0; i < cancelInst->numDestRegs(); i++) {
            auto dst = cancelInst->renamedDestIdx(i);
            if (dst->isFixedMapping()) continue;
            
            // 清除早期记分板标记
            earlyScoreboard[dst->flatIndex()] = false;
            
            // === 递归取消依赖者 ===
            for (auto iq : issueQues) {
                auto& depList = iq->subDepGraph[dst->flatIndex()];
                
                for (auto& [srcIdx, depInst] : depList) {
                    if (depInst->readySrcIdx(srcIdx)) {
                        // 该依赖指令被错误唤醒，需要取消
                        DPRINTF(Schedule, "[sn:%lli] Canceling dependent inst [sn:%lli]\n", 
                                cancelInst->seqNum, depInst->seqNum);
                        
                        depInst->issueQue->cancel(depInst);  // 取消指令
                        depInst->clearSrcRegReady(srcIdx);   // 清除就绪标记
                        cancelStack.push(depInst);           // 递归取消
                    }
                }
            }
        }
    }
    
    // === 清理流水线中的取消指令 ===
    // 从发射流水线中移除已取消的指令
    for (auto iq : issueQues) {
        for (int i = 0; i <= iq->getIssueStages(); i++) {
            auto& stage = iq->inflightIssues[-i];
            
            for (int j = 0; j < stage.size; j++) {
                if (stage.insts[j] && stage.insts[j]->canceled()) {
                    DPRINTF(Schedule, "[sn:%lli] Removed from issue pipeline stage %d\n", 
                            stage.insts[j]->seqNum, i);
                    stage.insts[j] = nullptr;
                }
            }
        }
    }
}
```

## 5. 配置和参数化

### 5.1 调度器配置 (configs/common/FUScheduler.py)

支持多种调度器配置：
- **ECoreScheduler**: 基础E-Core配置
- **KunminghuScheduler**: 昆明湖V3配置
- **KMHV3Scheduler**: V3优化配置
- **IdealScheduler**: 理想化配置

### 5.2 寄存器文件端口配置

通过 `rp` 参数配置寄存器读端口：
```python
# 整数寄存器读端口: [type_id(2bit)] [port_id(4bit)] [priority(2bit)]
def IntRD(id, p):
    return (0 << 6) | (id << 2) | (p)

# 浮点寄存器读端口
def FpRD(id, p):
    return (1 << 6) | (id << 2) | (p)
```

## 6. 关键代码位置

### 6.1 主要文件
- `src/cpu/o3/issue_queue.hh/cc`: 分布式发射队列实现
- `src/cpu/o3/inst_queue.hh/cc`: 兼容性包装器
- `configs/common/FUScheduler.py`: 调度器配置

### 6.2 重要函数
- `Scheduler::insert()`: 指令分发入口
- `IssueQue::selectInst()`: 指令选择逻辑  
- `Scheduler::specWakeUpDependents()`: 投机唤醒机制
- `IssueQue::checkScoreboard()`: 记分板检查
- `Scheduler::lookahead()`: 分发预测

### 6.3 数据流关键点
- `dispTable[opClass]`: OpClass到IssueQue的映射
- `wakeMatrix[srcIQ][dstIQ]`: 唤醒通道配置
- `subDepGraph[regIdx]`: 寄存器依赖关系
- `portBusy[]`: 端口占用管理

## 7. 调试和统计

### 7.1 调试标志
- `Debug::Schedule`: 调度相关调试信息
- `Debug::Dispatch`: 分发阶段调试信息

### 7.2 性能统计
- `iqstats->issueDist`: 发射指令分布
- `iqstats->portBusy`: 端口忙碌统计  
- `stats.exec_stall_cycle`: 执行停顿周期
- `stats.memstall_*`: 内存停顿统计
