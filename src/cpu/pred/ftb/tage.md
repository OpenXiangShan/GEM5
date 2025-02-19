### TAGE的基本结构：
   - 4级预测表(numPredictors=4)：
     - 每级表大小：2048项
     - Tag位数：每级8位
     - 历史长度：[8, 13, 32, 119] (逐级增加)
   - 基表(Base Table)：2048项
   - 每个表项包含：
     - valid: 有效位
     - tag: 标签
     - counter: 饱和计数器(-1,0表示NT, 1,2表示T)
     - useful: 替换策略使用位
### 预测逻辑
   1. 查找各级表(lookupHelper)：
      - 从高级到低级(长历史到短历史)查找
      - 计算每级的index和tag (getTageIndex/getTageTag)
      - 如果tag匹配，记录为provider
      - 只要找到一个匹配就停止查找
   
   2. 预测选择：
      - 如果没有表命中：使用base表预测(useAlt=true)
      - 如果有表命中：
        - 如果useAlt>0且主表弱预测(counter=-1或0)：使用base表
        - 否则：使用主表预测
   
   3. 最终预测：
      - counter>=0预测taken
      - counter<0预测not taken
### 更新逻辑
    1. 更新计数器：
      - 主表命中时：更新主表计数器(3位饱和计数器)
      - 使用base表时：更新base表计数器(2位饱和计数器)
   
   2. 更新useful位：
      - 当主表和base表预测不同时
      - 如果主表预测正确：useful=1
      - 如果主表预测错误：useful=0
   
   3. 分配新表项(当预测错误时)：
      - 从provider表的下一级开始
      - 优先选择useful=0的表项
      - 只分配一个表项
      - 新表项的counter初始化为：
        - 实际taken: counter=0
        - 实际not taken: counter=-1
   
   4. 更新useAlt选择器：
      - 当主表弱预测且主表与base表预测不同时
      - base表正确：useAlt增加
      - base表错误：useAlt减少

## 1. 关键数据结构

### 1.1 TageEntry (预测表项)
```cpp
struct TageEntry {
    bool valid;      // 表项是否有效
    Addr tag;        // 标签值
    short counter;   // 预测计数器，3位饱和计数器，范围[-4,3]
    bool useful;     // useful位，用于替换策略
}
```

### 1.2 TagePrediction (预测结果)
```cpp
struct TagePrediction {
    bool mainFound;      // 主表是否命中
    short mainCounter;   // 主表计数器值
    bool mainUseful;     // 主表useful位
    short altCounter;    // 备用表计数器值
    int table;          // 命中的表编号
    int index;          // 表中的索引
    Addr tag;           // 标签值
    bool useAlt;        // 是否使用备用预测
    bitset usefulMask;  // useful位掩码
    bool taken;         // 最终预测结果
}
```

### 1.3 主要成员变量
```cpp
// 预测表相关
vector<vector<vector<TageEntry>>> tageTable;    // 多级预测表[table][index][brIdx]
vector<vector<short>> baseTable;                // 基表[index][brIdx]
vector<vector<short>> useAlt;                   // 选择器[index][brIdx]，范围[-8,7]

// 历史相关
vector<FoldedHist> tagFoldedHist;              // tag的折叠历史
vector<FoldedHist> altTagFoldedHist;           // alt tag的折叠历史
vector<FoldedHist> indexFoldedHist;            // index的折叠历史

// 配置参数
vector<unsigned> tableSizes;                    // 各级表大小[2048,2048,2048,2048]
vector<unsigned> tableTagBits;                  // 各级表tag位数[8,8,8,8]
vector<unsigned> histLengths;                   // 各级历史长度[8,13,32,119]
```

## 2. 关键函数流程

### 2.1 预测流程 (putPCHistory)
```cpp
1. lookupHelper(): 查找各级预测表
   - getTageIndex(): 计算索引
   - getTageTag(): 计算标签
   - 从高到低查找各级表，找到第一个匹配的表项

2. 预测选择逻辑：
   - 无表命中：使用baseTable
   - 有表命中：
     - useAlt>0且主表弱预测：使用baseTable
     - 否则：使用主表

3. 最终预测：
   - counter>=0：预测taken
   - counter<0：预测not taken
```

### 2.2 更新流程 (update)
```cpp
1. 更新计数器：
   updateCounter()
   - 主表命中：3位饱和计数器[-4,3]
   - base表：2位饱和计数器[-2,1]

2. 更新useful位：
   - 条件：主表和base表预测不同
   - 主表正确：useful=1
   - 主表错误：useful=0

3. 分配新表项：
   - 条件：预测错误且非useAlt正确
   - 从provider表下一级开始
   - 优先选择useful=0的表项
   - 初始counter：taken=0，not taken=-1

4. 更新useAlt：
   - 条件：主表弱预测且与base表预测不同
   - base表正确：useAlt++ (最大7)
   - base表错误：useAlt-- (最小-8)
```

### 2.3 关键工具函数
```cpp
getTageIndex(pc, table)    // 计算表项索引
- 使用PC低位和折叠历史异或

getTageTag(pc, table)      // 计算表项标签
- 使用PC和两种折叠历史异或

getShuffledBrIndex(pc, brIdx) // 获取分支的物理索引
- 用于随机化分支到预测表的映射

updateCounter(taken, width, counter) // 更新饱和计数器
- 根据实际结果更新计数器值
```

## 3. 特殊机制

### 3.1 Useful位重置机制
```cpp
- 维护usefulResetCnt计数器
- 当分配失败时，根据可分配表项数调整计数器
- 计数器达到128时，重置所有表项的useful位
```

### 3.2 历史管理
```cpp
- 使用FoldedHist进行历史压缩
- 维护三种折叠历史：
  1. tagFoldedHist：用于计算tag
  2. altTagFoldedHist：用于计算alt tag
  3. indexFoldedHist：用于计算index
```

## 4. 统计信息
```cpp
struct TageBankStats
- 记录各级表的命中情况
- 记录预测器行为统计
- 包括useful位重置、分配成功/失败等
```



