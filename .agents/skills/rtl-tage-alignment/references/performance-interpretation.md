# TAGE 性能证据解释

按问题选择相关检查；只比较代码时无需准备 CI。

## 推荐分析顺序

### 1. 先看 CI summary 的总分和 benchmark 级变化

先回答：

- 总分涨还是跌
- 哪几个 benchmark 是主要贡献项

### 2. 再用 `gem5_data_proc` 做 benchmark 级 stats 对比

从具体 run 的 metadata、配置和 checkpoint 路径确定 compiler/profile、权重及统计窗口，再核对 `gem5_data_proc` 对应版本支持的选项。不要用当前 workflow 默认值推断历史 run，也不要固定套用 `--slice gcc12`。

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

### 模式 3：final allocation failure 降了，但 `cond_MPKI` 反而升了

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

- 你看到的 allocation failure 究竟是 probe-level 还是 final-level
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
