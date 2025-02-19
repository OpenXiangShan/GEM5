# GEM5 分离式前端 FTB(Fetch Target Buffer)模块

## 1. 模块概述

FTB(Fetch Target Buffer)模块是GEM5模拟器中分离式前端的核心组件，主要负责指令获取目标的预测和管理。该模块实现了高性能的分支预测和指令获取机制。

## 2. 核心数据结构

### 2.1 基础数据结构

#### FTBEntry (FTB表项)
FTB表项是FTB中的基本存储单元，包含以下主要字段：
- `tag`: FTB块的标签，由FetchStream的起始PC计算
- `slots`: 分支槽数组，最多包含两条分支信息
- `fallThruAddr`: FTB块的顺序执行地址
- `valid`: 表项是否有效

#### FetchStream (取指流)
取指流是前端预测的基本单位，记录了一个基本块的预测和执行信息：
- `startPC`: 起始PC
- `predEndPC`: 预测的结束PC
- `predBranchInfo`: 预测的分支信息
- `isHit`: 是否命中FTB
- `predFTBEntry`: 预测使用的FTB表项
- `exeBranchInfo`: 实际执行的分支信息
- `history`: 全局分支历史

#### FtqEntry (Fetch Target Queue表项)
FTQ表项对应一个32字节未对齐的基本块：
- `startPC`: 块起始PC
- `endPC`: 块结束PC
- `takenPC`: 跳转PC
- `target`: 目标地址
- `fsqID`: 对应的FetchStream ID

### 2.2 预测相关结构

#### BranchInfo (分支信息)
记录单条分支的基本信息：
- `pc`: 分支指令PC
- `target`: 分支目标地址
- `isCond`: 是否为条件分支
- `isIndirect`: 是否为间接跳转
- `isCall`: 是否为调用指令
- `isReturn`: 是否为返回指令
- `size`: 指令长度

#### FTBSlot (分支槽)
继承自BranchInfo，增加了预测相关字段：
- `valid`: 分支是否有效
- `alwaysTaken`: 是否总是跳转
- `ctr`: 饱和计数器

#### FullFTBPrediction (完整预测信息)
包含一次完整的预测所需的所有信息：
- `bbStart`: 基本块起始地址
- `ftbEntry`: FTB表项
- `condTakens`: 条件分支预测结果
- `indirectTarget`: 间接跳转目标
- `returnTarget`: 返回指令目标
- `predSource`: 预测来源
- `history`: 分支历史

### 2.3 辅助数据结构

#### LoopEntry (循环表项)
循环预测器的表项：
- `tripCnt`: 循环次数
- `specCnt`: 推测计数
- `conf`: 置信度
- `repair`: 是否需要修复

#### JAEntry (Jump Ahead表项)
跳转提前预测器的表项：
- `jumpAheadBlockNum`: 跳转提前的块数
- `conf`: 置信度

## 3. 主要功能模块

### 3.1 FTB核心实现 (DefaultFTB)
FTB的核心实现包含以下主要功能：

#### 基本结构
- 组相联结构：支持可配置的路数(numWays)和组数(numSets)
- 标签位宽可配置(tagBits)
- 每个表项支持多个分支槽(numBr)

#### 主要操作
1. **查找(lookup)**
   - 通过PC计算索引和标签
   - 在对应组中搜索匹配的表项
   - 返回命中的FTB表项信息

2. **预测(predict)**
   - 处理FTB命中和未命中情况
   - 为流水线各级填充预测结果
   - 支持条件分支和间接跳转的预测

3. **更新(update)**
   - 根据实际执行结果更新FTB表项
   - 维护分支槽的饱和计数器
   - 处理替换策略

### 3.2 TAGE预测器 (FTBTAGE)
TAGE(TAgged GEometric history length)预测器实现了多级分支历史的预测机制：

#### 基本结构
- 多个预测表(numPredictors)，每个表具有不同的历史长度
- 基础预测器(Base Table)作为备选预测结果
- 每个表项包含计数器(counter)和有用位(useful)

#### 主要功能
1. **预测过程**
   - 从最长历史长度开始查找匹配的表项
   - 使用折叠历史计算索引和标签
   - 根据useAlt位决定是否使用备选预测

2. **预测选择**
   - 主预测：来自TAGE表的预测结果
   - 备选预测：来自基础预测器的结果
   - 最终预测：根据useAlt位选择使用哪个预测结果

3. **更新机制**
   - 更新匹配表项的计数器和有用位
   - 动态分配新的表项
   - 维护预测器的准确性统计

### 3.3 分离式分支预测器 (DecoupledBPred)
分离式分支预测器实现了前端预测和执行反馈的解耦：

#### 主要特点
- 支持预测和执行的并行处理
- 维护预测状态队列
- 处理预测修复和重定向

#### 核心功能
1. **预测生成**
   - 生成基本块级别的预测
   - 管理预测元数据
   - 处理预测队列

2. **执行反馈**
   - 处理分支预测错误
   - 更新预测器状态
   - 管理预测修复

3. **预测修复**
   - 处理预测错误的恢复
   - 维护正确的预测状态
   - 更新相关预测器

## 4. 辅助模块

### 4.1 RAS(Return Address Stack)
返回地址栈实现了函数调用返回地址的预测：
- 支持嵌套函数调用
- 处理投机执行状态
- 实现预测修复机制

### 4.2 Loop Predictor
循环预测器用于预测循环的迭代次数：
- 记录循环的执行次数
- 预测循环的退出
- 维护循环预测的置信度

### 4.3 其他预测器
- Jump Ahead预测器：预测跳转提前
- ITTAGE：间接跳转目标预测器

## 5. 模块交互

### 5.1 预测流程
1. FTB查找和预测
2. TAGE方向预测
3. 间接跳转目标预测
4. 返回地址预测
5. 循环预测

### 5.2 更新流程
1. 执行结果反馈
2. 预测器状态更新
3. 预测修复处理
4. 统计信息收集

## 6. 使用说明

### 6.1 配置参数
- FTB大小和组织方式
- 预测器参数配置
- 历史长度设置

### 6.2 性能调优
- 预测器选择
- 参数优化
- 统计信息分析
