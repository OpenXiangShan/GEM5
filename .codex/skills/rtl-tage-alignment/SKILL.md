---
name: rtl-tage-alignment
description: 用于比较 XiangShan RTL 与 gem5 BTBTAGE/TAGE 在 allocation、useful、reset 和 update path 上的语义差异，并结合 CI、原始 stats 和 benchmark 结果定位性能变化来源。适用于用户要求检查“和 RTL 是否对齐”、分析 TAGE 相关性能变化、设计最小 A/B 验证时。
---

# RTL TAGE 对齐方法

## 概览

这个 skill 用来回答一类固定问题：

- gem5 的 BTBTAGE/TAGE 和 XiangShan RTL 到底哪里没对齐
- 某次“向 RTL 对齐”的修改为什么没有复现 RTL 的收益
- 一组 allocation/useful/reset 相关修改，究竟改变了什么统计，影响的是哪一层语义

建议和 [`ci-perf-analysis`](../ci-perf-analysis/SKILL.md) 配合使用：

- `rtl-tage-alignment` 负责语义对照和归因框架
- `ci-perf-analysis` 负责取 CI summary、archive 和 `gem5_data_proc` 数据

## 使用场景

适用于这些情况：

- 用户给出 RTL 路径或 RTL PR，要求检查 gem5 是否仍有同类 bug
- 用户说“RTL 修了这个以后收益很大，为什么模型没有”
- 用户想知道某个 BTBTAGE/TAGE commit 改的是不是 RTL 语义
- 用户想知道该看哪些计数器来判断 allocation/useful/reset 的行为

## 工作流

### 1. 先钉住两边的代码版本

至少明确下面几项：

- RTL 路径
  - 通常是
    `/nfs/home/yanyue/workspace/xs-env/XiangShan/src/main/scala/xiangshan/frontend/bpu/tage`
- RTL 是否已经 pull 到最新(默认pull 了)
- gem5 commit / branch
- 对应 CI run / 本地 checkpoint run

如果这些版本没钉住，后面的结论都容易漂。

### 2. 先比较代码语义，不要先看总分

比较顺序建议固定成下面 5 层：

1. allocation gating
2. victim eligibility
3. provider useful update
4. global useful reset cadence
5. stats naming / stats meaning

这样不容易把不同层次的问题混在一起。

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

典型易混点：

- old `updateAllocFailure`
  - 往往是 per-table-probe no-victim
- old `updateAllocFailureNoValidTable`
  - 往往才是最终完整 allocation failure
- new `allocProbeNoEligibleVictim`
  - 是 per-table-probe
- new `updateAllocFailure`
  - 是 final failure

如果不先对齐口径，很容易把结论做反。

## 性能分析时优先看的计数器

### 第一层：直接看 allocation / useful / mispred

- `allocateSuccess`
- final `allocateFailure`
- probe-level no-victim
  - 旧名或新名都要确认口径
- `resetUseful`
- `tage_update_mispred`
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

## 推荐分析顺序

### 1. 先看 CI summary 的总分和 benchmark 级变化

先回答：

- 总分涨还是跌
- 哪几个 benchmark 是主要贡献项

### 2. 再用 `gem5_data_proc` 做 benchmark 级 stats 对比

当前这套 CI 默认仍常见 `gcc12` 切片。  
处理 `gcc12-spec06-0.3c` 归档时，建议显式带：

```bash
--slice gcc12
```

如果以后 workflow 默认切到 `gcc15`，而本地 `run.py` 默认行为已经一致，可省略 `--slice`。

### 3. 再下钻原始 `stats.txt`

当这些情况出现时，必须回到原始 `stats.txt`：

- 新旧 commit 之间统计名改了
- summary 和直觉对不上
- benchmark 只有个别项漂，怀疑是局部波动
- 需要确认到底是 final failure 变了，还是 probe failure 变了

## 解释结果时的常见模式

### 模式 1：final failure 降了，但 success 没怎么降

更像是：

- 修掉了“不该发起的 allocation”
- 或修掉了“本来不该算成最终失败”的 case

不一定代表真实 allocation 压力变小了。

### 模式 2：probe failure 不降甚至升，但 final failure 降了

更像是：

- 最终还是能分进去
- 但中间搜索压力依旧存在
- 只是 bogus failure 变少了

### 模式 3：`allocateFailure` 降了，但 `cond_MPKI` 反而升了

更像是：

- 放宽 allocation 的同时增加了 churn
- provider 被更快冲掉
- conflict / alias 更高

### 模式 4：1-bit useful 和 2-bit useful 改动几乎不影响性能

如果同时满足下面条件：

- allocation 只判断 `useful == 0`
- `1/2/3` 都被视为“受保护”
- 普通 update 没有 per-entry decrement
- reset 会统一把 useful 清到 0

那么 useful 位宽本身很可能不是主矛盾。  
这时更值得怀疑的是 reset cadence。

## 常见坑

### 1. 不同 commit 的 stats 名字可能已经变了

分析前先确认：

- 你看到的 `allocateFailure` 究竟是 probe-level 还是 final-level
- 是否需要把旧名和新名手动映射

### 2. 不要把 “加计数器” 默认等同于 “完全不改语义”

有些“加 stats”的提交会顺手重构搜索逻辑。  
这类提交要先看 diff，再决定能不能当成 stats-only。

### 3. 先看代表 benchmark，再下总体结论

如果回退主要集中在：

- `gobmk`
- `sjeng`
- `gcc`
- `omnetpp`

优先拿这些 benchmark 的原始 `stats.txt` 做对照，而不是只看加权平均。

### 4. 对照 RTL 时，优先信代码和原始统计

如果出现：

- “总分方向和预期不一致”
- “某个 counter 降了但性能没涨”

优先检查：

- 代码语义是否真等价
- 统计口径是否真一致
- 原始 `stats.txt` 是否支持这个解释

## 输出建议

回答这类问题时，建议固定成下面顺序：

1. 这次对比的 RTL / gem5 版本
2. 代码上有哪些已知不对齐点
3. benchmark 级主要收益或回退
4. 关键计数器如何变化
5. 这些变化更像支持哪类根因
6. 下一步最小 A/B 应该怎么做

## 资源

### 推荐配合使用

- [`ci-perf-analysis`](../ci-perf-analysis/SKILL.md)
  - 负责拿 run、archive、`score.txt`、`gem5_data_proc` 结果

### 当前不需要额外脚本

如果后续这套对齐流程稳定下来，再考虑补：

- 统一提取 allocation/useful/reset 关键计数器的脚本
- 旧新 stats 命名映射表
