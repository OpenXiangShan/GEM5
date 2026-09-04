# GEM5 分支预测器模块说明

本目录包含了 GEM5 模拟器中的分支预测器实现，主要包括三种解耦前端设计：

- BTB-based：基于传统 BTB 的设计
- FTB-based：基于 Fetch Target Buffer 的设计
- Stream-based：基于指令流的设计

## 目录结构

```
pred/
├── btb/        # BTB相关实现
├── ftb/        # FTB相关实现
├── stream/     # Stream预测器相关实现
├── BranchPredictor.py  # 主要配置文件
└── README.md   # 本文档
```

## DecoupledBPUWithBTB 模块说明

### 1. 基本架构

接下来只关注 btb/ 文件夹内容！
DecoupledBPUWithBTB 是一个解耦的分支预测器设计，主要包含以下组件：

- UBTB (micro-BTB)：快速但简单的一级预测器
- BTB (Branch Target Buffer)：主要的分支目标缓冲器
- TAGE (Tagged Geometric Length) 预测器：用于方向预测
- ITTAGE：用于间接跳转预测
- RAS (Return Address Stack)：用于返回地址预测

这里每个预测器有继承类 TimedBaseBTBPredictor，包含如下关键函数

- putPCHistry: 根据 startAddr 作为起始预测地址，记录在 stagePreds 中作为预测结果
- getPredictionMeta：获取预测元数据，相当于 checkpoints, 存储预测时候的状态，预测错误时候回滚，指令提交时候验证
- specUpdateGHist/specUpdatePHist：推测更新 TAGE/ITTAGE/MGSC 等预测器的 folded history
- recoverHist/recoverPHist：squash（分支预测错误）后恢复 folded history，并用实际结果重放
- RAS 的 `specUpdateState`/`recoverState`：只更新返回栈内部状态，不属于 folded history 接口
- ABTB 的 `recoverState`：squash 后清理 ahead pipeline，不属于 folded history 接口
- update：用 commit stream 更新预测器内容，是准确更新
- commitBranch: 当前 FetchBlock 这条分支提交时候，统计数据

```
# 画出预测器继承关系, 几个预测器继承自TimedBaseBTBPredictor
DecoupledBPUWithBTB继承自BPredUnit

TimedBaseBTBPredictor
    ↓   ↓     ↓     ↓       ↓
UBTB   BTB   TAGE   ITTAGE   RAS


BPredUnit
    ↓ 
DecoupledBPUWithBTB
```

每个预测器有 numDelay 成员，表示在第几级流水线延迟，
其中 UBTB = 0, ITTAGE = 2, 其他预测器 = 1
表示 UBTB 能背靠背、无延迟的给出预测结果，其他预测器需要 1 级延迟，ITTAGE 需要 2 级延迟，分别产生 1 个 or2 个预测空泡

### 2. 主要参数配置

#### 2.1 基本配置

- `ftq_size`: 128, 取指目标队列大小
- `fsq_size`: 64, 取指流队列大小
- `maxHistLen`: 970, 历史长度
- `blockSize`: 32, 单个预测可覆盖的最大字节范围
- `numStages`: 3, 流水线级数

#### 2.2 BTB 配置

- 默认 BTB：
  - 2048 个表项
  - 20 位标签
  - 8 路组相连
  - 1 周期延迟

- 微型 BTB (UBTB)：
  - 32 个表项
  - 38 位标签
  - 32 路组相连
  - 0 周期延迟

#### 2.3 TAGE 配置

- 4 个预测表
- 每个表 4096 个表项
- 8 位标签
- 历史长度：[8, 13, 32, 119]

#### 2.4 ITTAGE 配置

- 5 个预测表
- 表大小：[256, 256, 512, 512, 512]
- 9 位标签
- 历史长度：[4, 8, 13, 16, 32]

### 3. 关键数据结构

分支预测以 FetchBlock 为单位，FetchBlock 是取指队列中一个元素，
对应一个 FullBTBPrediction，生成一个 FetchStream，再生成一个 FtqEntry，
一个 FetchBlock 包含一个或多个分支指令，也就是一个或多个 BTBEntry，
我们只记录曾经跳转过的分支指令，并记录跳转目标到 BTB 表中，

对应的结构体关系如下：

```
BranchInfo (基础分支信息)
    ↓
BTBEntry (BTB表项)
    ↓
FullBTBPrediction (完整预测， 包含多个BTBEntry)
    ↓
FetchStream (取指流， 包含多个BTBEntry)
    ↓
FtqEntry (取指目标， 包含预测为taken的第一个BTBEntry)
```

#### 3.1 分支信息 (BranchInfo)

```cpp
struct BranchInfo {
    Addr pc;                  // 分支指令地址
    Addr target;             // 目标地址
    bool isCond;            // 是否条件分支
    bool isIndirect;        // 是否间接跳转
    bool isCall;           // 是否函数调用
    bool isReturn;         // 是否函数返回
    uint8_t size;          // 指令大小
};
```

#### 3.2 BTB 表项 (BTBEntry)

继承自 BranchInfo，额外包含：

```cpp
struct BTBEntry : BranchInfo {
    bool valid;            // 表项是否有效
    int ctr;              // 预测计数器
    Addr tag;             // BTB 标签
};
```

#### 3.3 完整预测结果 (FullBTBPrediction)

整合所有预测器的预测结果：

```cpp
struct FullBTBPrediction {
    Addr bbStart;                     // 基本块起始地址
    std::vector<BTBEntry> btbEntries; // BTB 预测结果
    std::map<Addr, bool> condTakens;  // 条件分支预测
    std::map<Addr, Addr> indirectTargets; // 间接跳转目标
    Addr returnTarget;                // 返回地址预测
};
```

#### 3.4 取指流 (FetchStream)

管理一段连续指令序列的预测和执行信息：

```cpp
struct FetchStream {
    // 基本信息
    Addr startPC;          // 起始地址
    Addr predEndPC;        // 预测结束地址
    bool predTaken;        // 预测是否跳转
    
    // 预测状态
    BranchInfo predBranchInfo;  // 预测的分支信息
    std::vector<BTBEntry> predBTBEntries;  // BTB 预测结果，一个 FetchStream 包含多个 BTBEntry!
    
    // 执行结果，resolve 后更新
    bool exeTaken;         // 实际是否跳转
    BranchInfo exeBranchInfo;  // 实际分支信息
    
    // 统计信息
    int fetchInstNum;     // 取指令数
    int commitInstNum;    // 提交指令数
};
```

#### 3.5 取指目标队列项 (FtqEntry)

管理取指单元的取指请求：

```cpp
struct FtqEntry {
    Addr startPC;         // 取指起始地址
    Addr endPC;          // 取指结束地址
    Addr takenPC;        // 分支指令地址
    bool taken;          // 是否跳转
    Addr target;         // 跳转目标
    FetchStreamId fsqID; // 对应的取指流 ID
    
    // 循环相关
    bool inLoop;         // 是否在循环中
    int iter;           // 迭代次数
    bool isExit;        // 是否循环退出
};
```

```
[UBTB] ->   [BTB/TAGE/ITTAGE]
   ↓         ↓           ↓
   └──→ [FetchStream Queue] 
              ↓
     [FetchTarget Queue]
              ↓
     [Instruction Cache]
```

这些数据结构整体关系可以看做
每个预测器都会每拍生成预测结果，其中 FTB/uFTB 生成最核心的 FTBEntry,
然后其他预测器按需填入对应的方向或者别的信息，
共同生成每一级的 FullFTBPrediction, 最后 3 选 1 得到最终的 FullFTBPrediction（finalPred）；
下一拍会根据 finalPred 结果生成一项 FSQEntry 放入 FSQ 中
再下一拍会用 FSQEntry 生成一个 FTQEntry 放入 FTQ 中
最后 Fetch 函数会拿出 FTQEntry 来从 ICache 取指

### 4. 关键函数流程

#### 4.1 预测流程

1. `tryEnqFetchStream()`：
   - 检查是否有新的预测结果
   - 创建新的取指流
   - 更新预测状态

2. `processNewPrediction()`：
   - 使用各个预测器组件进行预测
   - 整合预测结果
   - 创建新的 FetchStream 条目

3. `tryEnqFetchTarget()`：
   - 将预测结果加入 FTQ
   - 更新取指状态

按照逻辑先后关系

1. components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);  调用各个组件的 putPCHistory 方法做出分支预测，预测结果记录在 predsOfEachStage 中;
2. generateFinalPredAndCreateBubbles();  // 用上一拍 3 级预测结果生成最终预测结果 finalPred，产生气泡
3. tryEnqFetchStream();    // 用 finalPred 结果生成 FSQentry 存入 FSQ, 本质上调用 processNewPrediction
4. tryEnqFetchTarget();    // 尝试入队到 FetchTarget 中，用上一拍 FSQ entry 生成一个 FTQ 条目存入 FTQ

#### 4.2 预测恢复流程

1. `controlSquash()`：
   - 处理分支预测错误
   - 恢复预测器状态
   - 更新历史信息

2. `nonControlSquash()`：
   - 处理非分支导致的流水线清空
   - 恢复预测器状态

3. `update()`：
   - 更新预测器状态
   - 提交已确认的预测结果

### 5. 特殊功能

支持以下可选功能：

- `enableLoopBuffer`: 循环缓冲区，用于提供循环指令
- `enableLoopPredictor`: 循环预测器，用于预测循环退出
- `enableJumpAheadPredictor`: 跳跃预测器，用于跳过不需要预测的块
- `alignToBlockSize`: 预测是否对齐到块大小边界
- 注意 DecoupledBPUWithBTB 默认不支持循环缓冲区，JumpAheadPredictor 也不支持

### 6. 调试支持

通过`bpDBSwitches`参数可以启用不同的跟踪选项，支持数据库形式的跟踪记录：

- 分支预测结果跟踪
- 循环预测器状态跟踪
- 预测器性能统计
