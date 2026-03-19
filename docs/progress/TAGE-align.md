# BTB TAGE RTL 对齐进展

## 背景

这轮工作的起点，是 XiangShan RTL 在 [PR #5677](https://github.com/OpenXiangShan/XiangShan/pull/5677) 修了一个 TAGE 分配相关 bug。用户观察到：

- RTL 修复后，每 GHz 分数提升大约 `+0.3`
- `allocationFailure` 和 `resetUseful` 都明显下降
- 但 gem5 模型侧没有复现出同方向收益，甚至有些改动还带来了回退

因此本轮工作的核心目标一直是：

1. 判断 gem5 的 BTBTAGE 是否还保留了和 RTL 同类的不对齐逻辑
2. 逐步把 gem5 和 RTL 的 TAGE 行为对齐
3. 用 0.3c CI 和原始 `stats.txt` 判断到底是哪类差异真正影响了性能

当前工作分支：

- `btb-tage-rtl-useful-sticky-align`
- RTL 本机路径 `/nfs/home/yanyue/workspace/xs-env/XiangShan/src/main/scala/xiangshan/frontend/bpu/tage`

## 时间线

### 1. `c7e1e3b99bb25ccd333f8000e26fc95181b5f07c`
`cpu: Align BTB TAGE allocation with RTL`

动机：

- RTL `#5677` 放宽了 allocate victim 选择条件
- 旧 gem5 仍然会把 `strong && !useful` 的陈旧项卡住，导致无法替换

修改：

- 将分配优先级改成：
  - `invalid`
  - `weak && !useful`
  - `any !useful`
- 当时也同步改了 `MicroTAGE`

结果：

- `allocation failure` 方向上确实改善
- 但性能没有变好，反而出现回退
- 后续分析发现：单独放宽 victim eligibility，会和 gem5 当时的 useful/update 逻辑形成一个“更激进但不等价于 RTL”的中间态

### 2. `ea6799f3975f1888f0d86f551a60883d30bebb36`
`cpu: Align BTB TAGE useful update with RTL`

动机：

- gem5 里原先多了两条 RTL 没有的 `useful = 0` 路径
  - humility reset
  - counter 变 weak 时清零

修改：

- 去掉上述两条 `useful = 0`
- 只保留更接近 RTL 的规则：
  - provider 预测正确
  - 且 provider 与 alt 不同
  - 才把 useful 置高

结果：

- 相比只改 allocation 的版本，性能回收了一部分
- 说明“额外的 useful 清零”确实会放大 churn
- 但仍未回到基线

代表 CI：

- run `23230428386`
- Int @3GHz 约 `54.7227`

### 3. `161c172724db278256910f20214cc4d7a111ac98`
`cpu: Align BTB TAGE useful state with RTL`

动机：

- 尝试进一步把 useful 状态机和 reset 语义向 RTL 靠拢

修改：

- 将 useful 从 1-bit `bool` 改成 2-bit 语义
- allocation 只把 `useful == 0` 视为可替换
- 同时修改 `usefulResetCnt` 的驱动方式：
  - allocation 成功时不再 `--`
  - 不再对每个 table probe fail 都 `++`
  - 改为只有“最终完整 allocation 失败”时才 `++`

结果：

- 性能进一步下降

代表 CI：

- run `23235115931`
- Int @3GHz 约 `54.3450`

后续判断：

- 这次回退最初看起来像 2-bit useful 导致
- 但后续更完整的 A/B 表明：真正更可疑的是 `usefulResetCnt` 的语义变化，而不是 useful 位宽本身

### 4. `6d3ba69ccdecf0b634814f48be95a45b359d2659`
`cpu: Add BTB TAGE allocation bucket stats`

动机：

- 想直接验证 allocation 路径里，究竟是哪些 victim case 在起作用

修改：

- 新增 probe 级 bucket 统计：
  - `has_invalid`
  - `has_weak_not_useful`
  - `has_strong_not_useful_but_no_weak_not_useful`
  - `all_useful_or_no_candidate`

经验教训：

- 这批统计后来发现是 **per-table-probe** 语义，不是 **per-allocation-attempt** 语义
- 因此不应该直接拿它和“最终完整分配失败”对比
- 这次提交虽然主要是加统计，但也重构了 victim 选择代码，所以并不是真正意义上的 “stats-only”

代表 CI：

- run `23235450311`
- Int @3GHz 约 `54.2656`

当前判断：

- 这次下降没有证据表明是“计数器本身加错了”
- 更像是局部 benchmark 漂移，尤其是 `omnetpp`

### 5. `cac85530e7b86694248fed7c89199d60a83f5255`
`cpu: Align BTB TAGE allocation gating with RTL`

动机：

- 继续检查 allocation 发起条件本身和 RTL 是否一致
- 看计数器发现GEM5 allocation 次数比RTL 多一些

修改：

- 补上 `highest table provider -> no allocate`
- 限制同一个 fetch block 最多只允许一次 allocation
- 同时整理统计命名：
  - 原 per-probe 的 `updateAllocFailure` 改成 `allocProbeNoEligibleVictim`
  - 原 final failure 的 `updateAllocFailureNoValidTable` 改成 `updateAllocFailure`

代表 CI：

- run `23237511277`
- Int @3GHz 约 `54.3271`

关键发现：

- 这次改动 **显著减少了最终完整 allocation failure**
- 但 **并没有稳定降低 allocation success**
- 很多 benchmark 上 success 不变，甚至略增
- probe 级 no-victim 计数也不一定下降

这说明它主要修掉的是：

- “本来 RTL 不该算成一次最终 alloc fail，但 gem5 以前会算进去”的 case

而不是简单粗暴地“少分配了很多次”

### 6. `51f151e18cc2ce566882d8948c977a6b5149e344`
`cpu: Restore 1-bit BTB TAGE useful state`

动机：

- 想把 `161c172` 拆开，验证 useful 位宽是否真的是主要问题

修改：

- 将 useful 恢复回 1-bit `bool`
- 但保留新的 reset 语义

代表 CI：

- run `23238614247`
- Int @3GHz 约 `54.2930`

关键结论：

- 与 `cac85530` 几乎没有本质差异
- 说明在当前这版 gem5 实现里：
  - `1-bit useful`
  - `2-bit useful`
  对性能的影响极小

也就是说：

- useful 位宽不是这一轮的主矛盾
- 真正更值得盯的是 reset cadence，以及其他 allocation/update 语义

## 这轮已经确认的 RTL 不对齐点

### 1. victim eligibility 原先不对齐

原先 gem5 过严，不允许替换 `strong && !useful`。  
RTL `#5677` 改完后，分配优先级明确是：

1. `invalid`
2. `weak && !useful`
3. `any !useful`

这个点已经在 gem5 对齐。

### 2. useful update 原先不对齐

gem5 曾额外存在两条 RTL 没有的 `useful = 0` 路径：

- humility reset
- weak-counter reset

这个点已经对齐，只保留“provider 证明自己比 alt 更有价值时置 useful”。

### 3. allocation gating 原先不对齐

这个点是比较重要的新增发现。

#### a. highest-table provider 不应再 allocate

RTL 有保护，gem5 原先没有。  
这会把一些本来“不该发起的 allocation”错误地记成 failure。

#### b. 同一个 fetch block 最多一次 allocate

RTL 有这个约束，gem5 原先可以对多个 branch 各自发起 allocation。

这个点已经对齐。

### 4. finalPred gate 仍未明确对齐

RTL 的 `needAllocate` 中包含：

- `(finalPred =/= actualTaken)`

但这个条件在 RTL 自身是否稳定、以及它对当前差异的贡献，暂时没有完全坐实。  
这一点目前没有在 gem5 中对齐。

### 5. reset cadence 仍与旧 gem5 不同

当前 gem5 保留的是较新的 reset 语义：

- 只有最终完整 allocation 失败时 `usefulResetCnt++`
- allocation 成功时不再 `--`

这和更早版本相比，是一个很大的行为变化。  
目前看，这个差异比 useful 宽度更可能解释性能上的长期偏移。

## 这轮分析中最重要的经验

### 1. 一定要区分 final failure 和 probe failure

曾经的统计口径很容易混淆：

- 旧 `updateAllocFailure`
  - 实际是 per-table-probe 的 “当前 table 没 candidate”
- 旧 `updateAllocFailureNoValidTable`
  - 才是最终完整 allocation failure

后来已经整理成：

- `allocProbeNoEligibleVictim`
- `updateAllocFailure`

以后分析时一定要先确认在比较的是哪一种。

### 2. useful 位宽本身不是主结论

当前 gem5 这版实现里：

- allocation 只关心 `useful == 0`
- 普通 update 没有 per-entry decrement
- reset 又会把 useful 统一打回 0

因此：

- 2-bit 的 `1/2/3`
- 1-bit 的 `true`

在“是否可替换”这件事上几乎等价。  
CI 也验证了这一点。

### 3. 真正更值得怀疑的是 reset 和 allocation 发起条件

到目前为止，更像主因的不是 useful 位宽，而是：

- allocation 是否发得过多
- final failure 的统计是否夹杂了本不该发起的 case
- useful reset 的节奏是否改变了全局 aging 行为

## 当前工作结论

到目前为止，可以相对稳地说：

1. gem5 原先确实存在和 RTL `#5677` 同方向的 victim 选择不对齐
2. 但把 victim eligibility 单独对齐，并不能自动复现 RTL 的大幅收益
3. useful 宽度不是主要矛盾
4. allocation gating 和 reset cadence 更值得继续深挖
5. 统计口径必须先统一，否则很容易把 probe-level pressure 和 final failure 混为一谈

## 建议的下一步

### 1. 单独隔离 reset 语义 A/B

如果继续实验，最干净的下一步是：

- 保持当前 allocation gating
- 保持 1-bit useful
- 只对 `usefulResetCnt` 的旧/新语义做单独 A/B

这样最容易坐实 reset cadence 的影响。

### 2. 继续做 RTL 语义对照时，优先按下面顺序检查

1. allocation gating
2. victim eligibility
3. provider useful update
4. reset cadence
5. stats naming / meaning

### 3. 继续分析时优先看这些指标

- `allocateSuccess`
- `updateAllocFailure`（当前语义下是 final failure）
- `allocProbeNoEligibleVictim`
- `resetUseful`
- `cond_MPKI`
- `BPAllWrong`
- `tage_update_mispred`
- `updateProviderNa`
- `updateAltDiffers`
- `updateUseAltCorrect`
- `updateUseAltWrong`

如果这些指标的方向彼此矛盾，优先相信：

- 原始 `stats.txt`
- benchmark 级变化
- 再结合代码语义做解释

