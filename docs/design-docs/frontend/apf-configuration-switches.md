# APF 配置开关

本次只拆分和参数化已有行为，不增加 ICache、ITLB、SC 或 RAS 资源模型，
也不改变备用路径恢复和训练流程。以下默认值针对 `configs/example/kmhv3.py`。

## TAGE 地址映射和 update 冲突

| 参数 | kmhv3 默认值 | 含义 |
| --- | --- | --- |
| `system.cpu[0].branchPred.tage.useBankedIndex` | `True` | index/tag 的 PC 位排除 bank 选择位 |
| `system.cpu[0].branchPred.tage.enableBankConflict` | `True` | 主路径预测与 resolved update 的 bank 争用，以及已接受 update 对 APF 的资源占用 |
| `system.cpu[0].branchPred.enable_apf_tage_resource_conflict` | `True` | APF 查询是否遵守 TAGE bank 占用 |
| `system.cpu[0].branchPred.enable_apf_mbtb_resource_conflict` | `True` | APF 查询是否遵守 MBTB 资源占用 |
| `system.cpu[0].apfShareDecode` | `True` | APF 是否使用主路径 Decode 剩余带宽 |

`useBankedIndex` 在 `BTBTAGE` 类中的基础默认值是 `False`；kmhv3 显式设为
`True`，保持此前 kmhv3 默认运行时的 index/tag 映射。以后切换
`enableBankConflict` 不再隐式改变映射。

只比较 update 冲突时，应固定 `useBankedIndex`，再切换
`enableBankConflict`。复现旧版本 `enableBankConflict=False` 的实验时，
还需显式设置 `useBankedIndex=False`；旧版本开启时则对应两者均为 `True`。

## H2P 参数

下表参数均位于 `system.cpu[0].branchPred` 下。

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `enable_h2p_table` | `True` | 启用 H2P 识别表，已有开关 |
| `enable_h2p_weak_confidence` | `False` | 入选 APF 前的 weak-confidence 硬过滤，已有开关 |
| `h2p_table_entries` | `128` | 对齐块 entry 总数，不是分支槽总数，已有参数 |
| `h2p_table_line_bytes` | `32` | 对齐块字节数，必须是至少为 2 的二次幂 |
| `h2p_table_ways` | `2` | 相联度，必须非零且整除 entry 总数 |
| `h2p_branches_per_entry` | `2` | 每个对齐块最多保存的分支数 |
| `h2p_counter_bits` | `3` | 饱和 counter 位宽，范围为 1 到 8 |
| `h2p_counter_threshold` | `3` | counter 大于等于该值时标记 H2P，须位于可表示的正值范围内 |
| `enable_h2p_provider_filter` | `True` | 是否限制新分配分支的 TAGE provider |
| `h2p_min_provider_table` | `2` | 新分配需要的最低 final provider table，沿用原编号，不重编号 |
| `h2p_age_insts` | `20000` | aging 间隔，已有参数 |

`h2p_table_ways=h2p_table_entries` 表示全相联；改变相联度不改变 entry
总数。每个块的分支槽数必须非零，且不能大于块内可容纳的 2B 指令数。
默认仍是 32B、2-way、每块两个分支、3-bit counter、阈值 3 和 T2+
provider 新分配过滤。命中已有分支时，仍允许递增 counter，不受 provider
过滤影响。关闭 provider 过滤时，没有 TAGE provider metadata 的新分支也可分配。

示例：

```bash
--param=system.cpu[0].branchPred.h2p_table_line_bytes=64
--param=system.cpu[0].branchPred.h2p_table_ways=4
--param=system.cpu[0].branchPred.h2p_counter_bits=4
--param=system.cpu[0].branchPred.h2p_counter_threshold=4
--param=system.cpu[0].branchPred.enable_h2p_provider_filter=False
```

## 配置校验和保留边界

- 非法 H2P 结构和 counter 配置在构造时拒绝，不能静默截断。
- 生成周期预算的定义、weak 硬过滤策略、计数器统计口径和提前预测器训练
  不在本次改动范围内，未为它们增加新行为。
- 即使现有争用开关全部开启，ICache/ITLB 读取仍是原来的理想模型。
