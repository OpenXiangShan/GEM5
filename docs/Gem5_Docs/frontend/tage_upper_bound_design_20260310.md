# UB-S(outcome) TAGE v1 设计（2026-03-10）

## 1. 目的

这份文档只定义一个可实现的 `v1`：

- 名称：`UB-S(outcome)`
- 目标：给 `gobmk_nngs_18098` 和 `gobmk_trevord_7886` 提供一个 **无 set/tag/way/replacement 竞争** 的 TAGE 上界
- 作用：回答 `8way` 到底离“当前 history-length 体系下的无竞争上界”还有多远

它不是 RTL 方案，也不是 oracle predictor。

## 2. v1 定义

`UB-S(outcome)` 的查找键定义为：

`(branchPC, exact outcome history prefix of length Li)`

其中：

- `branchPC`：该条件分支的 PC
- `Li`：第 `i` 张 TAGE 表的 history length
- `exact outcome history`：精确的 taken/not-taken 比特串，不做 folding

因此，v1 去掉了这些因素：

- set 冲突
- tag alias
- way 限制
- replacement / victim
- folded-history alias

但仍然保留这些因素：

- 固定的一组 history lengths
- provider / alternate 选择
- counter 更新
- base predictor 兜底

所以它是一个 **偏强但仍然像 TAGE 的上界**，而不是“纯结构上界”的严格最小定义。

## 3. 实现形态

第一版采用：

- `class BTBTAGEUpperBound : public BTBTAGE`

原因不是为了复用当前数组表项组织，而是为了复用：

- predictor 接线方式
- `FullBTBPrediction` / `FetchTarget` / `TageMeta` 生命周期
- decoupled frontend 的预测、更新、恢复入口

但要明确：

- **继承 `BTBTAGE` 不等于自动继承了 UB-S(outcome) 需要的 history 生命周期**
- 当前生产代码真正维护的是 path-history folding；`specUpdateGHist/recoverHist` 这部分对 `UB-S(outcome)` 不能直接沿用

## 4. 核心设计决策

### 4.1 必做项

`v1` 固定采用以下约束：

- 名称明确为 `UB-S(outcome)`，不再泛称 “upper-bound TAGE”
- `updateOnRead = false`
- exact outcome history 由子类自己维护
- `meta` 里不保存 `unordered_map` iterator
- key 使用 fixed-width 表示，不用 `dynamic_bitset` 直接做哈希 key
- 每张表预留 `reserve`
- 输出 `contexts_per_table` 与 `contexts_per_hot_branch`

### 4.2 v1 不做的事

- 不做 exact path history 版本
- 不做 oracle / infinite-context predictor
- 不先追求全 SPEC 可跑
- 不在 `BTBTAGE` 里打很多条件分支混跑

## 5. 数据结构

每张表维护一个哈希表：

```text
ubTable[i] : unordered_map<UBKey_i, UBEntry>
```

逻辑上：

- 一张表对应一个 history length `Li`
- 同一 `(branchPC, exact outcome history prefix Li)` 唯一映射到一个 `UBEntry`
- 没有 set/way/replacement

`UBEntry` 第一版只保留最小字段：

- direction counter
- useful bit / usefulness counter（若 provider/alt 逻辑仍依赖）

如果实现时发现 `u` 在 v1 里没有解释价值，可以先保留字段但弱化其作用；不要为了完全复刻当前 array-TAGE 的替换语义而复杂化 v1。

## 6. Key 设计

推荐定义两个固定宽度 key：

### 6.1 短历史表

对 `Li <= 64`：

- `pc`
- `hist_lo`
- `hist_len`

### 6.2 长历史表

对 `Li > 64`：

- `pc`
- `hist_words[N]`
- `hist_len`

这里的 `N` 不做动态长度，直接按当前配置支持的最大 history length 预留固定 word 数。

要求：

- 自定义 `hash`
- 自定义 `operator==`
- 避免把 STL 容器本身嵌进 key

## 7. History 生命周期

这是 reviewer 最后强调、也是 v1 最需要写清楚的点。

### 7.1 不能直接依赖现有 BTBTAGE 的 outcome-history 生命周期

当前 `BTBTAGE` 生产路径真正实现的是：

- `specUpdatePHist`
- `recoverPHist`

它维护的是 path-history folding。

而 `UB-S(outcome)` 需要的是：

- prediction 时可取到 exact outcome history snapshot
- squash 时可恢复并重放 exact outcome history

所以子类必须自己实现：

- `specUpdateGHist`
- `recoverHist`

并在自己的 `meta` 中保存与 exact outcome history 对应的 snapshot。

### 7.2 v1 的做法

建议在 `BTBTAGEUpperBound` 中额外维护：

- 一份精确的 speculative global outcome history
- 一份 prediction-time snapshot，写入 `meta`

规则：

1. prediction 时按当前 exact history 构造各表 key
2. `specUpdateGHist` 用最终预测方向推测更新 exact history
3. squash 时 `recoverHist` 先恢复 snapshot，再按真实结果重放

是否继续调用基类的 `specUpdatePHist/recoverPHist`：

- 可以保留，用于不破坏现有前端组件期望的 path-history 生命周期
- 但 `UB-S(outcome)` 的查找正确性不能依赖它

## 8. Meta 设计

第一版 `meta` 至少保存：

- prediction-time exact outcome history snapshot
- provider table id
- alternate table id
- provider key
- alternate key
- provider / alt / base 的决策结果

不要保存：

- `unordered_map` iterator
- 任何依赖容器不 rehash 的裸引用

原因很直接：

- 插入新 context 后 `unordered_map` 可能 rehash
- iterator / reference 稳定性不足以作为 update 阶段句柄

第一版更稳的方案是：

- `meta` 存 table + key + decision
- update 时按 key 二次查表

这也是 `updateOnRead=false` 的直接配套设计。

## 9. 预测与更新流程

### 9.1 预测

对每个表 `i`：

1. 取 `branchPC`
2. 从 exact outcome history 截取长度 `Li` 的前缀
3. 构造 `UBKey_i`
4. 从最长表到最短表寻找存在项

规则保持 TAGE 风格：

- 最长命中项作为 provider
- 次长命中项作为 alternate
- 都没有则落回 base predictor

### 9.2 更新

更新方向保持“像 TAGE”，但不再有分配失败：

1. 若 provider 存在，更新 provider counter
2. 若需要分配更长历史表项，直接在目标表 `emplace`
3. 不做 set 内择路
4. 不做 victim 选择
5. 不统计 alloc-fail

因此 `UB-S(outcome)` 的提升可直接理解为：

- 去掉跨分支、跨上下文的组织竞争后，当前 history-length 体系还能学到什么程度

## 10. 统计项

除常规 `IPC`、`branchMissPrediction`、`topMispredictsByBranch.csv` 外，v1 必须新增：

- `contexts_per_table`
- `contexts_per_hot_branch`
- `provider_table_distribution`

推荐再加：

- `base/provider/alt` 命中分布
- 每张表的 live contexts 峰值

这些统计是解释 `8way -> UB-S(outcome)` 差距的关键，不是可选项。

## 11. 验证范围

第一批只跑：

- `gobmk_nngs_18098`
- `gobmk_trevord_7886`

推荐顺序：

1. 先做短窗口功能验证
2. 再做 `20M warmup + 20M measured`
3. 只对热点分支输出 `2way / 4way / 8way / UB-S(outcome)` 对照

## 12. 结果解释规则

如果出现：

- `8way ≈ UB-S(outcome)`
  说明 ways/组织空间基本吃满，剩余误判更像 history 表达或分支语义上限

- `2way << 8way << UB-S(outcome)`
  说明仍有大量结构空间，继续做低表共存/去 alias 才有意义

- `2way ≈ 8way << UB-S(outcome)`
  说明问题不主要在 ways，本质更像当前 TAGE 组织方式没有抓到合适上下文

## 13. 最小实现建议

建议新增独立文件：

- `src/cpu/pred/btb/btb_tage_ub.hh`
- `src/cpu/pred/btb/btb_tage_ub.cc`

以及 Python 接线：

- `src/cpu/pred/BranchPredictor.py`

推荐实现顺序：

1. 定义 `UBKey` / `UBEntry` / custom hash
2. 搭 `BTBTAGEUpperBound : public BTBTAGE`
3. 实现 exact outcome history 的 `specUpdateGHist/recoverHist`
4. 实现 provider/alt 查找
5. 实现 update + 统计
6. 跑两个 gobmk slice

## 14. 当前结论

这份文档收敛后的结论只有一条：

**下一步最值得做的不是继续猜 replacement 或 alloc policy，而是先做 `UB-S(outcome)`，把 `8way` 与“无竞争上界”的距离量出来。**
