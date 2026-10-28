# astar_rivers_8610：正式 baseline TEA Top 5

只读取 `/tmp/tea-upperbound-20261028/final-twelve-tea-roi-v2/astar_rivers_8610/baseline/tea`；validation passed、20M warmup、两个完整块、末块提交 19,999,993、4,222,025 cycles，与原 CI 所列核心计数逐值一致。CSV 非负 PSV 权重加特殊 PSV −3 的 182 样本与采样周期守恒。每 PC 权重含 Computing/Stalled/Flushed/Drained，不能当纯 stall、可优化空间或速度收益。事件可重叠；无 PC×状态联合数据。原始 Top5 和各事件 Top5、ELF 指令见 [JSON](astar-formal-tea-baseline.json)。候选侧尚未读取，不构造前后结论。

| 排名 | PC / 实际 astar_rivers ELF 指令 | 总权重 / 周期 | 归因 CPI | 已核实的邻近依赖 |
|---|---|---:|---:|---|
| 1 | `0x1daa2 lhu a1,0(a5)` | 9.4605% | 0.019971 | 前有 `lw 1da8c/1da8e`、乘加得到地址；结果被 `1daa4 beq` 比较 |
| 2 | `0x1db4e sw s3,0(a2)` | 7.8301% | 0.016529 | `1db48 ld a1,8(a5)`、`1db4a sh3add` 形成写入地址，下一条 `1db52 sw s6,4(a2)` |
| 3 | `0x1db8e bne a7,a6,...` | 5.7876% | 0.012218 | 比较 `1db7a` 和 `1db8a` 两条半字加载结果 |
| 4 | `0x1db8a lhu a7,0(a5)` | 5.7444% | 0.012126 | 下一条 `1db8e bne` 消费结果 |
| 5 | `0x1daa4 beq a1,a4,...` | 4.4474% | 0.009389 | 消费 Top1 的 `1daa2` 加载值 |

五条全部准确映射到 **`way2obj::createway(...)[clone .constprop.0]`**，而非先前只凭旧输入选择的 `wayobj::makebound2`。此前 `0x1c1dc` 是同一 rivers ELF 中真实的 makebound2 指令，但它不是此切片此次 Top5，降为待新证据支持的候选，不能用旧候选替代实测热点。

实际可读 [Way2_.cpp](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/473.astar/src/Way2_.cpp:45) 包含网格 fillnum 检查、标记更新和邻居扩展；[:166](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/473.astar/src/Way2_.cpp:166) 起包含路径回溯搜索。静态 load/比较与这些结构一致，`1db4e` 的邻近两次 word store 只证实写入两个相邻字段，尚不能确认具体源码变量；无 DWARF，以上逐行对应仍是候选。检查热点循环是否在程序中固有地重复，以及有效指令/迭代，是必要边界：较高权重可能反映必要计算或搜索，而非硬件一定有可消除延迟。

全局状态 Computing 2,361,500、Stalled 1,508,813、Flushed 350,239、Drained 1,473 cycles。事件热点按全部周期作分母，不能互相相加：

| 事件 | 主要热点 / 事件权重 |
|---|---|
| FL_MB | `1db8e` 4.2176%；`1daa4` 3.6780%；`1dbe4` 0.5943% |
| ST_L1 | `1db8a` 2.5288%；`1daa2` 1.2849%；`1db08` 1.1382% |
| ST_TLB | `1daa2` 1.1661%；`1db8a` 0.5910%；`1db08` 0.2198% |
| ST_LLC | `1db8a` 0.3693%；`1db08` 0.2076%；`malloc 24b0c` 0.0131% |
| DR_L1 | 各最高 PC 约 0.005%；FL_MO 最高 `1db3a` 约 0.009%；DR_TLB、DR_SQ、FL_EX 无正权重 |

## 与已有 CI 证据结合的可检验方向

- **LSU 窗口/重放余量**：原 baseline RAWReplay 2,040、RAWQ 满 2,399 cycles；FE 23,374、11,818；四 load + FE 78,831、36,968；扩窗组合下降至 441、2,383（详见 [astar-ci-residual-notes.md](astar-ci-residual-notes.md)）。它们是全切片计数，未证明针对上述 Top5、也未证明等待位于关键路径。正在运行的 RAWQ 单轴应先核验正式 ROI，再与 candidate 同版本 TEA、PC 及 load/replay 等待时序配对；此处不新增模拟。
- **控制流质量与供给**：两条 top branch 的 FL_MB 权重可定位调研，但 FE-only 的提交分支错误 48,222→47,746 而 IPC 回退；不能用错误数直接解释。采 ROI outcome、各预测源和 branch resolve 时序，比较是否由历史、供给或重复搜索路径变化导致；其是否存在可改善的硬件余量待对照。
- **访存服务与固有计算区分**：`1daa2` 与 `1db8a` 有可重叠 L1/TLB/LLC 事件，`1db4e` 也可能只是循环中必要的 store/Computing 权重。需要同一动态迭代的 producer→load→branch / store 完成链，结合真实 cache demand miss/service、翻译和 LSU entry 占用，比较有用退休与重复执行；当前无法推断某条可消除的停顿。

资源边界：L2/L3 **总容量优先保持现有规模**；MSHR 或 bank 可在证据支持时作单轴放宽。若后续明确显示容量或预取污染，可扩大总容量作诊断并考虑预取器方向，但当前先完成 Top5 与已启动的 LSU 验证，不启动新实验。
