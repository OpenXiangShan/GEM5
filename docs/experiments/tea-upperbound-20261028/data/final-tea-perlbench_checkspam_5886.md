# perlbench_checkspam_5886 TEA残余诊断

20000000 executed warmup instructions; post-warmup ROI; approximately 19999994 measured instructions; not a weighted SPEC score; PC weights include Computing and overlapping events。状态互斥，事件可重叠。PC总权重包含Computing，不能视为可移除停顿。

| 状态CPI | Baseline | Combined | ΔCPI |
| --- | ---: | ---: | ---: |
| Computing | 0.147203 | 0.118646 | -0.028557 |
| Stalled | 0.047534 | 0.030495 | -0.017039 |
| Flushed | 0.009725 | 0.010863 | +0.001139 |
| Drained | 0.000664 | 0.000025 | -0.000639 |

| 事件归因CPI | Baseline | Combined | ΔCPI |
| --- | ---: | ---: | ---: |
| DR_L1 | 0.000049 | 0.000000 | -0.000049 |
| DR_TLB | 0.000000 | 0.000000 | +0.000000 |
| DR_SQ | 0.000000 | 0.000000 | +0.000000 |
| FL_MB | 0.009020 | 0.010166 | +0.001146 |
| FL_EX | 0.000000 | 0.000000 | +0.000000 |
| FL_MO | 0.000048 | 0.000127 | +0.000079 |
| ST_L1 | 0.012058 | 0.009256 | -0.002802 |
| ST_TLB | 0.007276 | 0.005566 | -0.001710 |
| ST_LLC | 0.011239 | 0.008638 | -0.002601 |

## 当前组合Top 5

按当前组合的PC总归因排序；占比包含Computing，不代表可优化空间。事件热点另列于JSON，不以Top 1决定方向。

| PC | 函数与指令 | Baseline总归因CPI | Combined总归因CPI | ΔCPI |
| --- | --- | ---: | ---: | ---: |
| 0x92e18 | S_regmatch lw	a5,0(a5) | 0.013938 | 0.009849 | -0.004089 |
| 0x96d10 | S_find_byclass.isra.0 bgeu	s2,a3,96cee <S_find_byclass.isra.0+0x380> | 0.004402 | 0.005189 | +0.000787 |
| 0x92f36 | S_regmatch lbu	a2,1(a4) | 0.004732 | 0.003511 | -0.001221 |
| 0x9bafa | Perl_runops_standard ld	a5,16(a5) | 0.003641 | 0.003067 | -0.000574 |
| 0x9667a | S_regtry sw	a5,0(a3) | 0.002070 | 0.001647 | -0.000423 |

基线Top 5：0x92e18、0x92f36、0x96d10、0x9bafa、0x92dfc。各事件只列非零热点；原始PC差异排名保留在JSON，差额不是单个组件的净收益。

四状态与CSV/PSV权重守恒检查通过。CSV没有PC×四状态联合字段；本表只对PC展示总权重，事件明细保留在JSON。守恒不证明观察等价，关闭TEA对照以单独验收记录为准；不得将PC差额视为单组件因果收益。
