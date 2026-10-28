# mcf_6753 TEA残余诊断

20000000 executed warmup instructions; post-warmup ROI; approximately 19999999 measured instructions; not a weighted SPEC score; PC weights include Computing and overlapping events。状态互斥，事件可重叠。PC总权重包含Computing，不能视为可移除停顿。

| 状态CPI | Baseline | Combined | ΔCPI |
| --- | ---: | ---: | ---: |
| Computing | 0.232725 | 0.223920 | -0.008806 |
| Stalled | 0.567694 | 0.510876 | -0.056818 |
| Flushed | 0.059200 | 0.058637 | -0.000563 |
| Drained | 0.000220 | 0.000025 | -0.000195 |

| 事件归因CPI | Baseline | Combined | ΔCPI |
| --- | ---: | ---: | ---: |
| DR_L1 | 0.000000 | 0.000000 | +0.000000 |
| DR_TLB | 0.000000 | 0.000000 | +0.000000 |
| DR_SQ | 0.000000 | 0.000000 | +0.000000 |
| FL_MB | 0.061379 | 0.060739 | -0.000640 |
| FL_EX | 0.000000 | 0.000000 | +0.000000 |
| FL_MO | 0.000114 | 0.000169 | +0.000055 |
| ST_L1 | 0.480721 | 0.432056 | -0.048666 |
| ST_TLB | 0.305570 | 0.282073 | -0.023498 |
| ST_LLC | 0.119867 | 0.065302 | -0.054565 |

## 当前组合Top 5

按当前组合的PC总归因排序；占比包含Computing，不代表可优化空间。事件热点另列于JSON，不以Top 1决定方向。

| PC | 函数与指令 | Baseline总归因CPI | Combined总归因CPI | ΔCPI |
| --- | --- | ---: | ---: | ---: |
| 0x10ff0 | main ld	a0,0(a0) | 0.074465 | 0.071793 | -0.002672 |
| 0x108e4 | main ld	t6,0(t6) | 0.062048 | 0.069614 | +0.007565 |
| 0x108d2 | main lw	t4,24(a5) | 0.121640 | 0.066731 | -0.054909 |
| 0x10fd4 | main ld	a0,0(a0) | 0.050335 | 0.050012 | -0.000322 |
| 0x10fee | main ld	a2,0(a4) | 0.034577 | 0.034688 | +0.000112 |

基线Top 5：0x108d2、0x10ff0、0x108e4、0x10fd4、0x10fee。各事件只列非零热点；原始PC差异排名保留在JSON，差额不是单个组件的净收益。

四状态与CSV/PSV权重守恒检查通过。CSV没有PC×四状态联合字段；本表只对PC展示总权重，事件明细保留在JSON。守恒不证明观察等价，关闭TEA对照以单独验收记录为准；不得将PC差额视为单组件因果收益。
