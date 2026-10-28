# astar_rivers_8610 TEA残余诊断

20000000 executed warmup instructions; post-warmup ROI; approximately 19999993 measured instructions; not a weighted SPEC score; PC weights include Computing and overlapping events。状态互斥，事件可重叠。PC总权重包含Computing，不能视为可移除停顿。

| 状态CPI | Baseline | Combined | ΔCPI |
| --- | ---: | ---: | ---: |
| Computing | 0.118075 | 0.115579 | -0.002496 |
| Stalled | 0.075441 | 0.064641 | -0.010799 |
| Flushed | 0.017512 | 0.020745 | +0.003233 |
| Drained | 0.000074 | 0.000001 | -0.000072 |

| 事件归因CPI | Baseline | Combined | ΔCPI |
| --- | ---: | ---: | ---: |
| DR_L1 | 0.000076 | 0.000000 | -0.000076 |
| DR_TLB | 0.000000 | 0.000000 | +0.000000 |
| DR_SQ | 0.000000 | 0.000000 | +0.000000 |
| FL_MB | 0.018374 | 0.021428 | +0.003053 |
| FL_EX | 0.000000 | 0.000000 | +0.000000 |
| FL_MO | 0.000024 | 0.000060 | +0.000036 |
| ST_L1 | 0.013612 | 0.009061 | -0.004551 |
| ST_TLB | 0.004782 | 0.003053 | -0.001728 |
| ST_LLC | 0.001309 | 0.000928 | -0.000381 |

## 当前组合Top 5

按当前组合的PC总归因排序；占比包含Computing，不代表可优化空间。事件热点另列于JSON，不以Top 1决定方向。

| PC | 函数与指令 | Baseline总归因CPI | Combined总归因CPI | ΔCPI |
| --- | --- | ---: | ---: | ---: |
| 0x1db4e | way2obj::createway(int, int, int, int, unsigned char*, pointt*&, int&) [clone .constprop.0] sw	s3,0(a2) | 0.016529 | 0.019757 | +0.003227 |
| 0x1db8e | way2obj::createway(int, int, int, int, unsigned char*, pointt*&, int&) [clone .constprop.0] bne	a7,a6,1db08 <way2obj::createway(int, int, int, int, unsigned char*, pointt*&, int&) [clone .constprop.0]+0x20c> | 0.012218 | 0.013223 | +0.001006 |
| 0x1daa4 | way2obj::createway(int, int, int, int, unsigned char*, pointt*&, int&) [clone .constprop.0] beq	a1,a4,1dbe0 <way2obj::createway(int, int, int, int, unsigned char*, pointt*&, int&) [clone .constprop.0]+0x2e4> | 0.009389 | 0.009999 | +0.000610 |
| 0x1daa2 | way2obj::createway(int, int, int, int, unsigned char*, pointt*&, int&) [clone .constprop.0] lhu	a1,0(a5) | 0.019971 | 0.009381 | -0.010590 |
| 0x1db3a | way2obj::createway(int, int, int, int, unsigned char*, pointt*&, int&) [clone .constprop.0] lw	a2,16(a5) | 0.004729 | 0.009314 | +0.004585 |

基线Top 5：0x1daa2、0x1db4e、0x1db8e、0x1db8a、0x1daa4。各事件只列非零热点；原始PC差异排名保留在JSON，差额不是单个组件的净收益。

四状态与CSV/PSV权重守恒检查通过。CSV没有PC×四状态联合字段；本表只对PC展示总权重，事件明细保留在JSON。守恒不证明观察等价，关闭TEA对照以单独验收记录为准；不得将PC差额视为单组件因果收益。
