# TAGE 语义检查与计数器

这是检查维度和统计名示例，不是跨版本固定的 RTL 规范；先核对对应 commit 的实现。

## 代码对照清单

### 1. allocation gating

优先检查：

- 是否只有真正需要 allocate 的分支才进入 allocation
- highest-table provider 是否禁止继续 allocate
- 同一个 fetch block 是否最多只允许一次 allocation
- 是否存在 `finalPred != actualTaken` 之类的额外 gate

这层决定：

- `allocateSuccess`
- final `allocateFailure`
- 是否会出现大量“本不该发起”的 allocation

### 2. victim eligibility

优先检查分配优先级是否一致：

1. `invalid`
2. `weak && !useful`
3. `any !useful`

要特别确认：

- gem5 是否还把 `strong && !useful` 卡住
- victim 搜索是第一命中、随机，还是按某个固定优先级

### 3. provider useful update

优先检查：

- provider 在什么条件下把 useful 置高
- gem5 是否额外存在 RTL 没有的 `useful = 0` 路径
- alt update 是否也会碰 useful

这层主要影响：

- provider 稳定性
- `altDiffers`
- `providerNa`
- `useAltCorrect/useAltWrong`

### 4. global useful reset cadence

最容易忽略，但往往影响很大。

优先检查：

- `usefulResetCnt` 在什么事件下 `++`
- allocation success 是否会让它 `--`
- 是按 table probe 计压，还是按最终 alloc fail 计压
- reset 时到底清的是单 entry、单 table 还是全表

如果 useful 平时没有局部衰减，这一层实际上就是整个 predictor 的 aging 机制。

### 5. stats naming / meaning

必须先搞清楚每个计数器是在统计什么。

先从当前对比 commit 的 `src/cpu/pred/btb/btb_tage.cc` 中确认 `ADD_STAT` 与递增位置。当前 checkout 的易混点是：

- `updateAllocFailure`：每个 table probe 找不到 eligible victim 时递增
- `updateAllocFailureNoValidTable`：完整搜索结束仍无法 allocation 时递增
- `updateAllocSuccess`：实际分配成功
- `updateResetU`：全局 useful reset
- `updateMispred`：进入对应 TAGE update 路径的误预测计数

这些名字可能随 commit 改变。若历史归档中出现 `allocProbeNoEligibleVictim` 等其他名字，必须回到该归档对应 commit 再建立映射；不要把这里的当前快照反向套给历史数据。

## 性能分析时优先看的计数器

### 第一层：直接看 allocation / useful / mispred

- `updateAllocSuccess`
- final failure（当前为 `updateAllocFailureNoValidTable`）
- probe-level no-victim（当前为 `updateAllocFailure`）
- `updateResetU`
- `updateMispred`
- `cond_MPKI`
- `BPAllWrong`

### 第二层：看 provider / alt 关系

- `updateProviderNa`
- `updateAltDiffers`
- `updateUseAltCorrect`
- `updateUseAltWrong`

这些指标很适合判断：

- provider 是更稳了，还是更弱了
- allocation 放宽以后是减少假性占坑，还是增加真实 churn

### 第三层：看前端症状

- `frontendBound`
- `badSpecBound`
- `branchMissPrediction`
- `frontendLatencyBound`

这些指标可以帮助判断：

- 问题是否主要还是 BPU
- 还是已经扩散成前端整体形态变化
