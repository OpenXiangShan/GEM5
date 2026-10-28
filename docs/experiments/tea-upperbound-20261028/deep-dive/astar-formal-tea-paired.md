# astar_rivers_8610 正式 TEA 端点配对

baseline 与最终 12 宽组合均验证 20M 预热、两个完整块、末 ROI 约 20M，且各自核心计数逐值匹配对应 CI。baseline 19,999,993 条 / 4,222,025 cycles，candidate 19,999,994 条 / 4,019,343 cycles；CSV、九事件和四状态守恒通过。结果为单切片两端点对照，包含前端、四 load、多个窗口容量等所有变化，**不是任何单项的因果分解**。原数据与所有事件 Top5 在 [JSON](astar-formal-tea-paired.json)。

总 CPI 0.211101→0.200967，Δ −0.010134；Computing −0.002496、Stalled −0.010799、Flushed +0.003233、Drained −0.000072 CPI。四状态互斥；事件标签可重叠。FL_MB +0.003053、ST_L1 −0.004551、ST_TLB −0.001728、ST_LLC −0.000381 CPI，不能相加为周期拆分。

| candidate Top5 PC | ELF 指令 | candidate 总归因 CPI | baseline CPI | ΔCPI |
|---|---|---:|---:|---:|
| `0x1db4e` | `sw s3,0(a2)` | 0.019757 | 0.016529 | +0.003227 |
| `0x1db8e` | `bne a7,a6,1db08` | 0.013223 | 0.012218 | +0.001005 |
| `0x1daa4` | `beq a1,a4,1dbe0` | 0.009999 | 0.009389 | +0.000610 |
| `0x1daa2` | `lhu a1,0(a5)` | 0.009381 | 0.019971 | -0.010590 |
| `0x1db3a` | `lw a2,16(a5)` | 0.009314 | 0.004729 | +0.004585 |

五条均落在 `astar_rivers.elf` 的 `way2obj::createway` clone。`1daa2 lhu→1daa4 beq`、`1db8a lhu→1db8e bne` 是明确的加载→分支数据依赖；`1db3a lw` 后与 `1db3c lw` 比较，后续 `1db48 ld→1db4a sh3add→1db4e sw` 构成另一条地址路径。实际 [Way2_.cpp](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/473.astar/src/Way2_.cpp:45) 的网格标记/邻居扩展及 [:166](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/473.astar/src/Way2_.cpp:166) 的回溯循环是语义候选；无 DWARF 单行对应，不能硬分配具体源码语句。

candidate FL_MB 主要 PC 为 `1db8e` 0.010403、`1daa4` 0.008747、`1dbe4` 0.001753 CPI；ST_L1 为 `1db8a` 0.003669、`1db08` 0.001811、`1da8c` 0.001485；ST_TLB 为 `1daa2` 0.001307、`1db8a` 0.000891、`1da8c` 0.000349；ST_LLC 为 `1db8a` 0.000545、`1db08` 0.000317、`malloc 24b0c` 0.000020。它们是标签归因，非互斥停顿。Top PC 总权重含 Computing 等四状态，热点可能是固有算法工作或重复执行，不等于可回收周期。

端点中 `1daa2` 权重降低与全局 Stalled 降低并存，`1db3a/1db4e` 和两条分支权重却上升；这只定位值得追踪的动态路径。需要正式 ROI 的生产者、replay、branch resolve 与有效提交事件相互对齐，判断是否为可调 LSU 等待、控制时序变化或固有搜索；单独 RAWQ 结果见 [RAWQ 单轴](astar-rawq-ablation-analysis.md)。总 L2/L3 容量优先保持，MSHR/bank可在证据支持时单轴；污染或容量证据充分时也可扩大容量诊断。
