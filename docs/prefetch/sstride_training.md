# KMH 对齐配置中的 SStride 训练

`XSCompositePrefetcher.sstride_require_first_issue_tlb_hit` 要求标量 load **首发时 TLB 命中**才有资格训练 L1D SStride；首发 miss 后的重放不补训。
通用默认值为 `False`；`configs/example/kmhv3.py` 选用的 KMH 对齐配置将其设为
`True`。这不是关闭 SStride，也不修改 TLB、ROB 或 MSHR。

| O3 标量 demand load 到达 L1D 的原因 | L1D 结果 | 对齐模式 |
| --- | --- | --- |
| 首发，翻译当次完成 | miss | 可训练 |
| 首发，翻译当次完成 | 命中来源为 SStride 的预取行 | 可训练，不限首次命中该行 |
| 首发，翻译当次完成 | 普通命中或命中其他预取器的行 | 不训练 |
| 首发 DTLB miss，翻译完成后重放 | 任意 | 不补训 |
| 首发后因 bank conflict 等原因快重放或慢重放 | 任意 | 这次重放不训练 |

O3 的 S0 遇到延迟翻译会设置 `TLBMissReplay`，跳过该次 S1 请求；翻译完成后的
`retryMem` 会标记重放来源，不能把最终获得物理地址当成首发命中。LSQ 在每次
发送数据包时重新标记当次资格；向量、异常和不可缓存请求没有资格。缓存观察点
保存包上的值，而不是在训练时读取可能已重放的指令状态。立即训练和排队训练
使用同一规则。对齐模式需要有此标记的 O3 load；没有标记的请求不获得资格。

“可训练”不是“已入表”或“已发包”：共享 TrainFilter 仍可按行去重或因满而丢弃，
而且未获 SStride 资格的观察事件仍可供其他子预取器使用。这里没有移植 RTL 的
独立训练队列、步长表置信度或历史地址位宽。

## 对照与观测

在相同 `kmhv3.py` 命令上追加
`--param 'system.cpu[0].dcache.prefetcher.sstride_require_first_issue_tlb_hit=False'` 可恢复旧规则：
在已有公共过滤之后，非 store 的 miss 或任意预取来源的首次命中都可训练。

- `strideTrainCount`：实际送给 SStride 的训练事件数，不是预取请求数。
- `strideTrainFilteredIssue`：到达 SStride 判断点、满足 miss 或 SStride 行命中条件，
  但没有合格首发标记的事件数。它包含多种重放等原因，不等于 DTLB miss 数。
- 使用 `gem5.opt` 的 `LoadPipeline,HWPrefetch,XSCompositePrefetcher` 调试标志，
  可用动态指令序号关联 S0 的 `setTLBMissReplay`、TrainFilter 出队和 SStride
  训练/拒绝记录。比较开关时应固定 checkpoint、内存模型和测量段。
