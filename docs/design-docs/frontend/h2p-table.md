# H2P 分支分类表

H2P 表根据已经提交的条件分支误预测记录，标记容易再次误预测的分支。
当前接入只记录分类和统计，为后续前端优化提供依据，不改变预测结果或流水线时序。

## 查询、训练和老化

- 创建 FTQ 条目时查询预测路径上的条件分支，跳过起始 PC 之前的分支，
  并在预测 taken 的分支处停止。分类结果保存在 FTQ 中。
- 条件分支提交且发生误预测时，分配表项或递增已有分支的饱和计数器。
  正确预测不递减计数器，因此阈值不是“连续误预测次数”。
- 每提交 `h2p_age_insts` 条指令，对有效分支计数器减一；归零的分支槽失效。
  SMT 下共用该表和提交指令计数，表按 PC 索引。
- 同一对齐块的分支共享表项。替换优先回收没有活跃分支的表项，其次使用组内 LRU。

## 配置

参数位于 `DecoupledBPUWithBTB`，可以通过 `--param` 覆盖。

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `enable_h2p_table` | `True` | 启用查询、训练、老化和分类统计 |
| `h2p_table_entries` | 128 | 对齐块表项总数 |
| `h2p_table_line_bytes` | 32 | 对齐块字节数 |
| `h2p_table_ways` | 2 | 组相联度 |
| `h2p_branches_per_entry` | 2 | 每表项的分支槽数量 |
| `h2p_counter_bits` | 3 | 饱和计数器位宽 |
| `h2p_counter_threshold` | 3 | H2P 分类阈值 |
| `h2p_age_insts` | 20000 | 老化间隔，以提交指令数计 |
| `enable_h2p_provider_filter` | `True` | 根据预测时的最终 TAGE provider 限制新分配 |
| `h2p_min_provider_table` | 2 | 允许新分配的最小 provider 表号，从零计数 |
| `enable_h2p_weak_confidence` | `False` | 对 PC 表命中结果再做置信度过滤 |

Provider 过滤只限制新分配，已有分支仍可训练。无 provider 元数据时不允许新分配。
置信度过滤接受 TAGE 主 provider 的非饱和计数器，即现有 SC 元数据中的 low 或 mid；
它不改变 SC 的置信度定义。缺失元数据被拒绝，无主 provider 时保留 TAGE 的 low 标记。

`kmhv3.py` 提供 `--disable-h2p-table` 和 `--enable-h2p-weak-confidence` 快捷选项。

## 统计口径

`system.cpu.branchPred.h2p*` 同时记录 PC 表原始分类和置信度过滤后的分类。
TP/FP/FN/TN 在 FTQ 条目提交时，根据预测时保存的标记和实际提交分支结果累计。
Coverage 为 `TP / (TP + FN)`，precision 为 `TP / (TP + FP)`。
查询数包括后来被 squash 的预测；分类准确性只统计提交的条件分支，二者分母不同。
表训练和 FTQ 退休可能跨越 stats reset 边界，短区间内的计数不一定逐项相等。
