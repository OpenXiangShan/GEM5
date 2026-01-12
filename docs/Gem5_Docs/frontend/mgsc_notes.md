# MGSC 预测器备忘

面向本仓库 BTB 侧的 MGSC（Multi-Geometric Statistical Corrector）总结：设计特点、关键结构、改进思路、统计计数器、以及便于验证的敏感小测试。

## 1. MGSC 设计要点
- 核心角色：TAGE 做“主判官”，MGSC 做“量化纠错”。MGSC 拿到 TAGE 的预测与置信度后，基于多类独立的历史特征表（几何长度）求和，给出一个带权的分数；只有当分数绝对值高于动态阈值时才覆盖 TAGE，否则保持原判。
- 特征多样性（降维防冲突）：  
  - **G** 全局方向历史：捕获长程相关；  
  - **P** 路径历史：区分同一分支的不同来路；  
  - **L** 局部（per-PC）历史：解决每个分支的重复模式；  
  - **BW** 后向历史：强化循环/尾递归偏好；  
  - **I/IMLI** 迭代计数：对“第 N 次迭代取反”类模式敏感；  
  - **Bias** 静态偏置：兜底偏好。  
  这些表使用折叠历史压缩到较短索引位宽，降低端口和存储压力，同时减少与 TAGE 的直接结构耦合。
- 决策与门控：各表计数器 percsum 做线性求和，并用简化权重（1x/2x 档）缩放，合成 `total_sum`。阈值由全局+PC 两级组成，并根据 TAGE 置信度动态放宽/收紧（高/中/低置信度对应 |sum| > thres/2, /4, /8）。这样在 TAGE 自信时，需要更强证据才覆盖。
- 自适应更新：仅在 SC 预测错或低置信时更新，避免破坏已稳定的正确判决；权重只在“该表是否关键到翻转决策”时调整；阈值在 SC 与真实冲突时上调，吻合时下调，使覆盖策略自动收敛到合适激进度。

## 2. 关键数据结构（参考 `src/cpu/pred/btb/btb_mgsc.hh/.cc`）
- `MgscPrediction`：一次预测的完整快照，包含：  
  - 总分 `total_sum`，是否使用 MGSC (`use_mgsc`)，最终 taken 判定；  
  - TAGE 原始预测与置信度（高/中/低）；  
  - 各表索引、raw/scaled percsum、`weight_scale_diff`（权重翻倍/归零是否翻转决策）、组合阈值。  
  这些字段既驱动训练，也被统计模块消费。
- 折叠历史：`index{G,P,Bw,L,I}FoldedHist`，把长历史折叠成较短索引用；Bias 索引还混入 TAGE 主预测、低置信标志，形成轻量级“条件偏置”。
- 预测与权重表：六类计数器矩阵 `bw/l/i/g/p/biasTable[table][index][way]`，存储方向倾向；对应的 `*_WeightTable[index]` 只用 PC hash，提供 1x/2x 缩放权重（-32..31，经简化映射）。
- 阈值：全局 `updateThreshold`（单计数器）+ PC 表 `pUpdateThreshold`（按 PC hash 的多计数器），共同决定覆盖门槛，随误差自适应。
- 元数据：`MgscMeta` 在 fetch 时缓存预测与折叠历史，用于后续恢复/回滚，保证推测一致性。

## 3. 新增统计计数器及含义（已合入）
路径：`system.cpu.branchPred.mgsc.*`
- SC/TAGE 关系：`scCorrectTageWrong`、`scWrongTageCorrect`、`scCorrectTageCorrect`、`scWrongTageWrong`、`scUsed`、`scNotUsed`、`scPredCorrect/Wrong`、`predHit/Miss`。
- 权重关键性：`*WeightScaleDiff`（bw/l/i/g/p/bias）——该表权重翻倍/去掉会翻转决策的次数。
- Raw percsum 符号正确率：`*PercsumCorrect/Wrong` —— 该表自身方向是否与真实一致。
- 阈值方向：`pcThresholdInc/Dec`、`globalThresholdInc/Dec` —— SC 与真实冲突多则递增，反之递减。
- 按 TAGE 置信度分桶的 SC 使用/绕过：`scHigh/Mid/LowUseCorrect/Wrong`、`scHigh/Mid/LowBypass`。

解读小贴士：
- 某表 `WeightScaleDiff` 高且 `PercsumCorrect` 高：表在“起关键作用”。若后者低，则可能误导。
- `pc/globalThresholdDec` ≫ `Inc`：SC 表现好，门槛在下降；反之说明冲突多。
- `scCorrectTageWrong` ≫ `scWrongTageCorrect`：SC 对 TAGE 有正纠错价值。

## 4. 改进方向（未实现，仅供参考）
1) 权重档位更细（0.5/1/1.5/2 等），提升相关性刻画精度。  
2) 权重表索引加入部分历史哈希（而非仅 PC），降低别名。  
3) 阈值更新考虑 TAGE 置信度（高置信错时更快抬阈，低置信错时慢抬），减少误覆盖。  
4) 更丰富的可观测性：按表/置信度输出热分支热点，或导出 per-PC 纠错热度。

## 5. MGSC 敏感小测试（C 版，放在 'nexus-am/tests/frontendtest/mgsc_test/tests/`）
已提供源码，可按现有 Makefile 通配编译：
- `long_period_flip.c`：长周期偶发翻转 + 噪声。期望 I/G/P 纠偏远距稀疏翻转。  
- `xor_dependency.c`：B2 方向 = 上一轮 B0^B1，跨分支相关。期望路径/局部/全局组合纠错。  
- `alias_many_branches.c`：16 个相邻分支，各有不同小周期，制造索引/标签冲突。期望 MGSC 通过局部/路径缓解别名。

使用建议：开/关 MGSC 对比 `condMiss`、`mgsc.scCorrectTageWrong/scWrongTageCorrect`、各表 `*PercsumWrong` 与阈值增减，定位是哪类表在纠错或噪声。必要时可缩短 TAGE 历史或减小表尺寸以放大差异。 
