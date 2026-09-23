# Partial Store 数据策略

## 行为

L1D 的 partial store miss 仍先发送 `StorePermReq` 获取写权限。L1D 在请求上携带
`skip-data-fetch` 决策，L2 只在本地 miss 时使用该决策：

- `always-skip`：保持原 gem5-udp 行为，L2 向下转发 `StorePermReq`，不取数据。
- `always-fetch`：L2 miss 转成带 split 标记的 `ReadExReq`。L3 先返回权限，随后
  返回完整数据；权限立即返回 L1D，完整数据安装在 L2。
- `adaptive`：由 L1D Predictor 选择上述两条路径，冷启动使用 fetch。

L2 full hit 和 partial hit 均直接授予权限，不主动补全，不受该策略影响。

## Predictor

Predictor 是每个 L1D 一个全局滑动窗口。默认参数：

- window：64 个发生 eviction 的 partial-store-origin cacheline。
- minimum samples：16。
- positive 比例达到 75% 时进入 skip-data 模式。
- positive 比例低于 50% 时退出 skip-data 模式。

训练只发生在 L1D 真正 eviction 时。样本定义如下：

- positive：cacheline 最初由 partial store 分配，最终完全由 stores 补满，且期间
  从未因 load、未覆盖 write、snoop 或 partial fill 请求缺失数据。
- negative：eviction 时仍为 partial，或者期间曾请求/合并缺失数据。

普通 invalidation、clean/writeback 操作不单独产生训练样本。

## 配置

`configs/example/kmhv3.py` 支持：

```text
--partial-store-data-policy {always-fetch,always-skip,adaptive}
--partial-store-predictor-window 64
--partial-store-predictor-min-samples 16
--partial-store-predictor-enter-percent 75
--partial-store-predictor-exit-percent 50
```

默认策略为 `always-skip`，以保持未显式启用实验时的 gem5-udp 行为。

相关统计以 `partialStorePredict*`、`partialStore*Outcomes`、
`partialStoreFetch*` 和 `partialStoreSkip*` 命名，位于 L1D cache stats 中。
