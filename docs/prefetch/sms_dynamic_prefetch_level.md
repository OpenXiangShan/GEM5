# SMS 动态预取层级

## 背景

SMS 的 PHT 为每个相对 offset 保存一个 3-bit 饱和计数器。原有实现只用
`counter > 0.5 saturation` 判断是否生成预取，并把所有通过判断的请求发送到
`pht_pf_level` 指定的同一个 cache 层级。该方式简单，但不能区分不同置信度的
offset，也不能区分一次 PHT lookup 是否由新 region 的首次访问触发。

本 feature 在保留原有固定层级模式的同时，增加可选的动态层级模式。动态模式
按 offset 独立读取 PHT counter，并结合 trigger access 将请求发送到 L2、L3，
或直接过滤。它不改变 SMS offset 的发送顺序，也不增加新的 PHT lookup 触发源。

## 配置语义

`XSCompositePrefetcher.enable_sms_dynamic_prefetch_level` 是唯一的模式开关：

- `false`：固定 SMS 预取层级。PHT counter 的饱和度大于 0.5 时生成请求，所有
  请求使用 `pht_pf_level`。这是原有行为。
- `true`：动态 SMS 预取层级。每个 offset 根据 PHT counter 和 trigger access
  独立选择目标层级；`pht_pf_level` 不参与动态决策。

该开关是 SimObject 参数，不提供命令行参数。配置文件直接决定产品 profile：

- `configs/example/kmhv3.py` 通过 KMH alignment profile 明确关闭该 feature。
- `configs/example/idealkmhv3.py` 在系统构建后明确开启该 feature。

三个 confidence threshold 的默认值为：

- `sms_pht_high_conf_threshold = 6`
- `sms_pht_medium_conf_threshold = 4`
- `sms_pht_low_conf_threshold = 3`

阈值作用于 PHT 3-bit counter 的原始值，必须满足
`high >= medium >= low`。

## 动态决策

这里的 trigger access 指 `actLookup()` 为当前访问分配了新 ACT region，即该次
PHT lookup 对应一个 region 的首次访问。它不是 stride 触发，也不是“该 cache
line 第一次访问”的泛化概念。

默认策略如下：

| PHT counter | Trigger access | Non-trigger access |
| --- | --- | --- |
| `counter >= high` | L2 | L3 |
| `medium <= counter < high` | L2 | 过滤 |
| `low <= counter < medium` | L3 | 过滤 |
| `counter < low` | 过滤 | 过滤 |

直观上，trigger access 允许中高置信度请求更靠近 core，并把较低置信度请求放到
L3；non-trigger access 只保留高置信度请求，并放到 L3，避免重复访问持续向 L2
注入流量。

## 实现边界

### PHT confidence 更新

动态模式按 ACT region 中每个 offset 的 first touch 更新 PHT，使用
`phtUpdatedBits` 防止同一个 offset 在 early update 和 eviction update 中重复加
分。feature 关闭时仍走独立的 legacy update 路径，以保持固定层级模式的历史
行为。

### 请求缓冲

一个 SMS region 中的不同 offset 可能被分配到不同层级。因此
`PrefetchFilter::Entry` 除了总 `region_bits` 外，还保存 L1/L2/L3 三组 destination
bit mask。固定模式插入请求时，会自动把全部 `region_bits` 映射到
`pht_pf_level`，所以其他使用 `PrefetchFilter` 的预取器不需要改变调用方式。

同一 offset 如果再次以更近的层级插入，buffer 保留更近的目标层级，并允许已
发送的较远层级请求在更近层级重新发送。

## 统计与 review 入口

动态与固定模式都记录最终分类结果：

- `smsPhtIssuedL1`
- `smsPhtIssuedL2`
- `smsPhtIssuedL3`
- `smsPhtFiltered`

主要代码入口：

- `src/mem/cache/prefetch/Prefetcher.py`：模式与 threshold 参数；
- `src/mem/cache/prefetch/sms.cc`：trigger 判定、confidence 分类和 PHT 更新；
- `src/mem/cache/prefetch/prefetch_filter.cc`：按 offset 保存和发送 destination；
- `configs/common/PrefetcherConfig.py`：KMHV3 固定层级配置；
- `configs/example/idealkmhv3.py`：Ideal KMHV3 动态层级配置。
