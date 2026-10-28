# mcf_6753 正式 20M TEA 配对

baseline / 最终 12 宽 candidate 的 20M warmup + 约20M ROI 均通过 validation、difftest、TEA 守恒，selected CI counters 分别逐值相同；IPC 1.1630→1.2603。baseline TEA OFF validation 已通过并与 CI 的 selected counters 匹配；candidate OFF 尚无 validation，因此不能宣称完整 observer equivalence。端点同时改变 FE、4load、窗口，以下差异不分摊到任何单项。

| CPI | baseline | candidate |
|---|---:|---:|
| Computing | .232725 | .223920 |
| Stalled | .567694 | .510876 |
| Flushed | .059200 | .058637 |
| Drained | .000220 | .000025 |
| DR_L1 / DR_TLB / DR_SQ | 0 / 0 / 0 | 0 / 0 / 0 |
| FL_MB | .061379 | .060739 |
| FL_EX | 0 | 0 |
| FL_MO | .000114 | .000169 |
| ST_L1 | .480721 | .432056 |
| ST_TLB | .305570 | .282073 |
| ST_LLC | .119867 | .065302 |

PC 总权重含 Computing；PSV 事件位可重叠，故权重不是可省 CPI，事件不能相加。candidate 前五总权重 .292838 CPI，约占全部采样周期 36.91%，不代表优化上限。

| candidate 排名 / 实际 ELF 指令 | 总权重 baseline→candidate CPI | 事件 baseline→candidate CPI | 静态依赖与可验证余量 |
|---|---:|---|---|
| 1 `0x10ff0 ld a0,0(a0)` | .074465→.071793 | ST_L1 .073497→.070838；ST_TLB .063037→.060925 | `node→basic_arc` 指针加载后读 cost，再与 pred potential 加减；基本地址依赖是程序工作。先核实同一动态链翻译/缓存请求与等待，不能从事件值预测可恢复量。 |
| 2 `0x108e4 ld t6,0(t6)` | .062048→.069614 | ST_L1 .059353→.067333；ST_TLB .024949→.029182 | arc.tail→tail.potential，再与 head.potential/cost 算 red_cost；前后地址依赖固定，但两分支是否存在可利用并行/隐藏等待需请求时序证据。 |
| 3 `0x108d2 lw t4,24(a5)` | .121640→.066731 | ST_L1 .120821→.065946；ST_TLB .076240→.045997；ST_LLC .115739→.063066 | arc 扫描取 ident→判别是否处理；扫描和筛选必要。等待下降值得追踪，但端点多轴变化和事件重叠排除单轴因果归因。 |
| 4 `0x10fd4 ld a0,0(a0)` | .050335→.050012 | ST_L1 .048164→.047727；ST_TLB .021101→.020719 | 另一入口的 node.basic_arc→cost 加载链，接 orientation 的加减分支；仅凭几乎不变的总权重无法判定瓶颈是否可改善。 |
| 5 `0x10fee ld a2,0(a4)` | .034577→.034688 | ST_TLB .000018→.000015 | node.pred→potential，再与 basic_arc.cost 加减；显式事件很小，且总权重含 Computing，目前无证据将它作为首要等待优化目标。 |

ELF 为 profile 的 `mcf.elf`，SHA256 `818d286599edb64b3493bb75d083cbf2bc8fb6e76a39c3da79aeb62cdf828766`；上述都是 LTO `main` 中指令。arc 字段偏移与 `pbeampp.c` 的扫描、node 字段偏移与 `mcfutil.c:refresh_potential` 相符，属于结构语义候选；无原 ELF 应用 DWARF 精确行号，当前可读源码是否与历史版本逐字一致仍未证明。

candidate 各事件热点：ST_L1 依次 10ff0 .070838、108e4 .067333、108d2 .065946、10fd4 .047727、10e04 .029628；ST_TLB 10ff0 .060925、108d2 .045997、108e4 .029182、10fd4 .020719、108e8 .017355；ST_LLC 108d2 .063066 居首，后续各点均低于 .00051。FL_MB 为 108f2 .014574 及 sort_basket 分支 11762 .008411、11780 .007814、11794 .005997、1177c .005948；FL_MO 顶点 1087e .000095。DR_L1/DR_TLB/DR_SQ/FL_EX candidate 为零，无有效热点。所有事件各自 Top5、baseline 对照和未归因信息见 JSON。

DTLB `l1InitialLookupMisses` 的增加不能直接等同唯一容量失败：PTW miss queue 重试能重新进入初次查找，四组间 PTW memory request 与 L1 淘汰计数基本稳定，具体重复比例尚无直接计数；ST_TLB 也是观察到的归因事件，不是可恢复 CPI。优先以实际地址/请求和翻译事件对齐验证 10ff0 / 108d2 的因果路径，再针对 LSU 内部 entries 或 DTLB 单轴反证。R8 Sbuffer 和 data bank CI 已开始，不能从这里预设结果或盲目扩容量；L2/L3 总容量默认维持，MSHR/bank 可单轴，有明确容量或预取污染证据时可用扩大容量作诊断。本次未读未完成 DTLB512，也未新增模拟。
