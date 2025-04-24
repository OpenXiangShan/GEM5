# GEM5 O3 CPU Fetch Stage Analysis

## Overview

The Fetch stage is the first pipeline stage in the GEM5 O3 processor model. It is responsible for fetching instructions from the instruction cache and passing them to the Decode stage. In the XiangShan GEM5 customized version, the Fetch stage implements a decoupled frontend design to align with the XiangShan processor architecture.

## Key Interfaces with Other Pipeline Stages

### 1. Time Buffer Interfaces

The Fetch stage communicates with other pipeline stages through a time buffer mechanism, which models the delay in communication between different stages:

```cpp
TimeBuffer<TimeStruct> *timeBuffer;  // Main time buffer for communication

// Wires from other stages to Fetch
TimeBuffer<TimeStruct>::wire fromDecode;   // For stall signals and instruction counts
TimeBuffer<TimeStruct>::wire fromRename;   // For stall signals
TimeBuffer<TimeStruct>::wire fromIEW;      // For stall signals and branch resolution
TimeBuffer<TimeStruct>::wire fromCommit;   // For squash signals and interrupts

// Wire to Decode stage
TimeBuffer<FetchStruct>::wire toDecode;    // For sending fetched instructions
```

### 2. Interface with Decode Stage

The Fetch stage forwards fetched instructions to the Decode stage through the `toDecode` wire:

```cpp
// In tick() method:
// Send instruction packet to decode
if (numInst) {
    toDecode->insts = std::move(insts);
    toDecode->size = numInst;
    wroteToTimeBuffer = true;
}
```

The Decode stage can send stall signals back to Fetch via the `fromDecode` wire:

```cpp
// In checkSignalsAndUpdate() method:
// Check if the decode stage is stalled
if (fromDecode->decodeStall) {
    stalls[tid].decode = true;
}
```

### 3. Interface with Commit Stage

The Commit stage sends several important signals to the Fetch stage:

- **Branch misprediction signals**: Indicate a branch was mispredicted and the pipeline needs to be squashed
- **Interrupt signals**: Indicate an interrupt needs to be processed
- **Drain signals**: For simulation control

```cpp
// In checkSignalsAndUpdate() method:
// Check for squash from commit
if (fromCommit->commitInfo[tid].squash) {
    DPRINTF(Fetch, "[tid:%i] Squashing from commit.\n", tid);
    squash(fromCommit->commitInfo[tid].pc,
           fromCommit->commitInfo[tid].doneSeqNum,
           fromCommit->commitInfo[tid].squashInst, tid);
}

// Check for commit's interrupt signals
if (fromCommit->commitInfo[tid].interruptPending) {
    interruptPending = true;
}
```

### 4. Interface with IEW Stage

The IEW (Issue/Execute/Writeback) stage provides branch resolution feedback to the Fetch stage:

```cpp
// In checkSignalsAndUpdate() method:
// Check for squash from IEW (mispredicted branch)
if (fromIEW->iewInfo[tid].squash) {
    DPRINTF(Fetch, "[tid:%i] Squashing from IEW.\n", tid);
    squash(fromIEW->iewInfo[tid].pc,
           fromIEW->iewInfo[tid].doneSeqNum,
           fromIEW->iewInfo[tid].squashInst, tid);
}
```

### 5. Branch Predictor Interface

The Fetch stage interacts with the branch predictor to determine the next PC to fetch:

```cpp
bool lookupAndUpdateNextPC(const DynInstPtr &inst, PCStateBase &pc) {
    // Access branch predictor and update PC
    bool predicted_taken = getBp()->predict(inst, pc, inst->pcState());
    // ... additional logic ...
    return predicted_taken;
}
```

## Key Data Structures

### 1. Fetch Buffer

The fetch buffer holds raw instruction data fetched from the instruction cache:

```cpp
uint8_t *fetchBuffer[MaxThreads];     // Raw instruction data
Addr fetchBufferPC[MaxThreads];       // PC of first instruction in buffer
bool fetchBufferValid[MaxThreads];    // Whether buffer data is valid
unsigned fetchBufferSize;             // Size of fetch buffer in bytes
Addr fetchBufferMask;                 // Mask to align PC to fetch buffer boundary
```

The fetch buffer works as a temporary storage between the instruction cache and the instruction queue. Instructions are fetched from the instruction cache in cache-line-sized chunks and stored in the fetch buffer.

### 2. Fetch Queue

The fetch queue stores the processed dynamic instructions before they are sent to decode:

```cpp
std::deque<DynInstPtr> fetchQueue[MaxThreads];  // Queue of fetched instructions
unsigned fetchQueueSize;                        // Maximum size of fetch queue
```

### 3. Memory Request Structures

For I-cache access management:

```cpp
RequestPtr memReq[MaxThreads];        // Primary memory request
RequestPtr anotherMemReq[MaxThreads]; // Used for unaligned access
PacketPtr firstPkt[MaxThreads];       // First packet for I-cache access
PacketPtr secondPkt[MaxThreads];      // Second packet for unaligned access
std::pair<Addr, Addr> accessInfo[MaxThreads];  // Address info for cache access
```

### 4. Status Tracking Structures

```cpp
// Overall fetch status
enum FetchStatus { Active, Inactive } _status;

// Per-thread status
enum ThreadStatus {
    Running, Idle, Squashing, Blocked, Fetching, TrapPending,
    QuiescePending, ItlbWait, IcacheWaitResponse, IcacheWaitRetry,
    IcacheAccessComplete, NoGoodAddr, NumFetchStatus
} fetchStatus[MaxThreads];

// Stall tracking
struct Stalls {
    bool decode;
    bool drain;
} stalls[MaxThreads];

// Stall reason tracking
std::vector<StallReason> stallReason;
```

### 5. Branch Prediction Structures

```cpp
branch_prediction::BPredUnit *branchPred;  // Main branch predictor
branch_prediction::stream_pred::DecoupledStreamBPU *dbsp;  // Stream predictor
branch_prediction::ftb_pred::DecoupledBPUWithFTB *dbpftb;  // FTB predictor
branch_prediction::btb_pred::DecoupledBPUWithBTB *dbpbtb;  // BTB predictor
```

### 6. Loop Buffer Structures

```cpp
branch_prediction::ftb_pred::LoopBuffer *loopBuffer;  // Loop buffer
bool enableLoopBuffer;                                // Loop buffer enable flag
unsigned currentLoopIter;                             // Current loop iteration counter
bool currentFetchTargetInLoop;                        // If current fetch is in a loop
```

## Core Function Workflow

### 1. Main Fetch Cycle

```
tick()
  |
  +--> checkSignalsAndUpdate()  // Check for control signals from other stages
  |
  +--> fetch()  // Perform actual instruction fetching
        |
        +--> getFetchingThread()  // Select thread to fetch from
        |
        +--> fetchCacheLine()  // Access I-cache
        |
        +--> lookupAndUpdateNextPC()  // Consult branch predictor
        |
        +--> buildInst()  // Create dynamic instructions
        |
        +--> writeToTimeBuffer()  // Send instructions to decode
```

### 2. Signal Processing Workflow

```
checkSignalsAndUpdate()
  |
  +--> Process stall signals from Decode
  |
  +--> Process branch resolution from IEW
  |
  +--> Process squash signals from Commit
  |
  +--> Process interrupt signals from Commit
```

### 3. I-Cache Access Workflow

```
fetchCacheLine()
  |
  +--> Create memory request(s)
  |
  +--> Send request(s) to I-cache
  |
  +--> Wait for response in recvTimingResp()
        |
        +--> Process received data
        |
        +--> Update fetch buffer
        |
        +--> Update fetch status
```

### 4. Branch Prediction Handling

```
lookupAndUpdateNextPC()
  |
  +--> Query branch predictor
  |
  +--> Update next PC based on prediction
  |
  +--> Track prediction metadata
```

## Decoupled Frontend Implementation

The decoupled frontend design separates branch prediction from instruction fetching, using specialized queues:

```cpp
// Check if using decoupled frontend
bool isDecoupledFrontend() { return branchPred->isDecoupled(); }

// Different predictor types
bool isStreamPred() { return branchPred->isStream(); }
bool isFTBPred() { return branchPred->isFTB(); }
bool isBTBPred() { return branchPred->isBTB(); }

// Track if FTQ is empty
bool ftqEmpty() { return isDecoupledFrontend() && usedUpFetchTargets; }
```

In the decoupled design, the branch predictor works ahead of the fetch unit, generating fetch targets that are stored in the Fetch Target Queue (FTQ). The fetch unit then consumes entries from this queue.

## Key Implementation Details

### 1. Instruction Cache Interface

The Fetch stage has its own port to access the instruction cache:

```cpp
class IcachePort : public RequestPort {
    // Handles timing requests to I-cache
    virtual bool recvTimingResp(PacketPtr pkt);
    
    // Handles retry signals from I-cache
    virtual void recvReqRetry();
};

IcachePort icachePort;
```

### 2. Address Translation Handling

The Fetch stage handles instruction address translation:

```cpp
class FetchTranslation : public BaseMMU::Translation {
    // Called when translation completes
    void finish(const Fault &fault, const RequestPtr &req,
                ThreadContext *tc, BaseMMU::Mode mode);
};

// Event to handle delayed translation results
class FinishTranslationEvent : public Event {
    // Process translation result
    void process();
};

FinishTranslationEvent finishTranslationEvent;
```

### 3. SMT Thread Selection

The Fetch stage has multiple thread selection policies:

```cpp
// Thread selection policies
ThreadID getFetchingThread();  // Main policy selection function
ThreadID roundRobin();         // Round robin policy
ThreadID iqCount();            // Based on instruction queue count
ThreadID lsqCount();           // Based on load/store queue count
ThreadID branchCount();        // Based on branch count
```

## Performance Monitoring and Statistics

```cpp
struct FetchStatGroup : public statistics::Group {
    // Stall statistics
    statistics::Scalar icacheStallCycles;
    statistics::Scalar tlbCycles;
    statistics::Scalar idleCycles;
    statistics::Scalar blockedCycles;
    
    // Instruction statistics
    statistics::Scalar insts;
    statistics::Scalar branches;
    statistics::Scalar predictedBranches;
    
    // Performance metrics
    statistics::Formula idleRate;
    statistics::Formula branchRate;
    statistics::Formula rate;
    
    // Frontend performance metrics
    statistics::Formula frontendBound;
    statistics::Formula frontendLatencyBound;
    statistics::Formula frontendBandwidthBound;
};
```

## XiangShan-Specific Enhancements

1. **Decoupled Frontend Designs**: Support for BTB, FTB, and Stream-based prediction
2. **TAGE, ITTAGE and Loop Predictor**: Advanced branch prediction aligned with XiangShan
3. **Instruction Latency Calibration**: Timing calibrated to match Kunminghu hardware

## Advanced Features

1. **Loop Buffer**: Caches loop instructions for energy efficiency
2. **Pipelined I-cache Access**: Allows overlapping multiple I-cache accesses
3. **Fetch Throttling**: Controls fetch rate based on backend pressure


## 代码分析
checkStall函数
这个函数检查特定线程是否因drain操作而暂停。当CPU正在drain（排空）时，该线程的fetch会被stall。

updateFetchStatus函数
这个函数负责更新fetch阶段的状态：
检查所有活跃线程的状态，如果任一线程处于Running、Squashing或IcacheAccessComplete状态，则将fetch阶段设为Active
否则将其设为Inactive，并通知CPU

squash函数
处理来自commit阶段的squash操作：
调用doSquash进行实际的squash操作
通知CPU移除不在ROB中的指令

tick函数
这是fetch阶段的主循环函数，每个时钟周期执行一次：
更新各线程的fetch状态统计
对每个活跃线程检查信号并更新状态
执行fetch操作，获取指令
处理中断（在FullSystem模式下）
发起流水线式的I-cache访问请求
将fetch队列中的指令发送到decode阶段，受decodeWidth限制
根据stall原因设置相应的stallReason
测量前端气泡（frontend bubbles）作为性能分析数据
分支预测器相关操作（支持StreamPred、FTBPred和BTBPred三种不同类型的预测器）

fetch函数
这是实际执行指令获取的函数，它调用fetchInstructions来获取指令。
主要特点和机制
多线程支持：代码支持多线程处理，通过ThreadID来区分不同线程的状态和操作。
流水线控制：
通过stall机制控制流水线暂停
支持流水线式的I-cache访问
分支预测：
支持多种分支预测器类型（Stream、FTB、BTB）
预测器每周期更新，尝试为fetch提供目标地址
性能统计：
收集各种指标来分析性能
实现了Intel TopDown方法来测量前端气泡
stall原因跟踪：
详细记录并传递stall原因到decode阶段
区分不同类型的stall（完全stall、部分stall）
状态管理：
完整的fetch状态转换逻辑
与CPU其他阶段的信号交互
这部分代码展示了GEM5模拟器中O3处理器模型fetch阶段的核心实现，重点关注了性能、流水线控制和与其他阶段的交互。代码中包含了详细的调试输出（DPRINTF），便于跟踪执行流程和状态变化。