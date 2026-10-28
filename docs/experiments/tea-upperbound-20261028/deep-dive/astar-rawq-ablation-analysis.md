# astar_rivers_8610：RAWQ 单轴

在 **R4 四 load + 理想前端、原窗口容量**背景下，`RAWQEntries` 从 56→128；该单轴已正常达到 20M 预热＋20M ROI，difftest 开启，两个完整块。完整 INI 除 RAWQ 和 DRAM 文件路径外一致（路径内容的等价性沿用已有 validation，未在本次独立重算）。[审计 JSON](astar-rawq-ablation-analysis.json) 保留统计及路径。

| 指标 | RAWQ 56 | RAWQ 128 |
|---|---:|---:|
| 正式提交指令 | 19,999,994 | 19,999,996 |
| cycles | 4,204,810 | 4,189,578 |
| CPI | 0.210240563 | 0.209478942 |
| RAWQueueFull / RAWReplay 事件 | 78,831 | 0 |
| RAWQueueFullCycles | 36,968 | 0 |
| IEW IQFull 事件 | 591,349 | 587,296 |
| 提交分支错误 | 47,530 | 47,604 |

净 ΔCPI −0.000761621，IPC +0.363579%。该单轴说明在原窗口背景下，扩 RAWQ 足以消除本点观测到的 RAWQ 满及 replay，并带来较小的条件净收益；满周期可能与其他事件重叠，不能直接换算为可回收 CPI。

这**不能证明**大 ROB/PRF/LQ/SQ/RARQ 等其余扩容后的背景中 RAWQ 扩容不重要，也不能把 +0.364% 当成最终七项容量组合收益中 RAWQ 的独立份额。若要回答必要性，需要未来在最终大窗口背景中单独将 RAWQ 128→56，固定其余参数并检查同一 ROI；此处没有该对照。单点不能推广到整个 astar 或 SPEC。端点 TEA 只定位相关 PC，不证明这些 RAW replay 必然对应某个热点。
