# mcf_6753 正式 baseline TEA

20M 预热后 ROI 19,999,999 committed、17,196,781 cycles；validation、difftest、TEA 守恒及 selected CI 计数逐值一致。candidate 尚无 validation，未读取为正式 ROI；TEA OFF 观察等价尚未证明。

全局 CPI：Computing .232725、Stalled .567694、Flushed .059200、Drained .000220；事件（可重叠）ST_L1 .480721、ST_TLB .305570、ST_LLC .119867、FL_MB .061379、FL_MO .000114，DR_L1/DR_TLB/DR_SQ/FL_EX 均 0。Top5 总归因 .343065 CPI，分母取全部 17,196,781 sample 为 39.899%；取已归因权重 17,192,160.002568 为 39.909%。特殊样本 cancelled=289、pending=0、unresolved=4,332。PC 总权重包含 Computing，事件位可重叠，不可将 .343 CPI 或事件和视为可节省周期。

| PC / ELF 指令 | 总 CPI | 事件 CPI（可重叠） | 静态链与结构语义 |
|---|---:|---|---|
| 0x108d2 `lw t4,24(a5)` | .121640 | ST_L1 .120821、ST_TLB .076240、ST_LLC .115739 | arc 指针加偏移→ident→`blez`；对应 arc 扫描/筛选结构候选。顺序遍历和筛选本身是必要工作；是否可隐藏访问等待须用轨迹及干预。 |
| 0x10ff0 `ld a0,0(a0)` | .074465 | ST_L1 .073497、ST_TLB .063037、ST_LLC .000735 | `ld a0,48(a5)`→本条读 cost→与 pred potential 加减；node/basic_arc 指针追踪是静态依赖，动态可重叠度未证明。 |
| 0x108e4 `ld t6,0(t6)` | .062048 | ST_L1 .059353、ST_TLB .024949 | `ld t6,8(a5)`→tail potential→sub/add→分支；tail 与 head 两链可否并行、前端和 LSU 是否限制需测。 |
| 0x10fd4 `ld a0,0(a0)` | .050335 | ST_L1 .048164、ST_TLB .021101、ST_LLC .000518 | 另一入口的 basic_arc→cost 链，接 orientation 加减；不应假定因统计高就可移除指针依赖。 |
| 0x10fee `ld a2,0(a4)` | .034577 | ST_TLB .000018 | `ld a4,24(a5)`→pred potential→与 cost 加减；相关显式事件小，总权重不能充作内存收益。 |

ELF SHA256 `818d286599edb64b3493bb75d083cbf2bc8fb6e76a39c3da79aeb62cdf828766`，这些 PC 均为 ELF `main` 内 LTO 代码；arc 字段布局与 `pbeampp.c` 扫描结构相符，node 偏移和子节点遍历与 `mcfutil.c:68-95` refresh_potential 相符，属于强结构语义候选，旧 ELF 没有应用源码行 DWARF，当前源与历史源完全一致未证明。

事件热点：ST_L1/ST_TLB 前列均为 108d2、10ff0、108e4、10fd4 等；ST_LLC 首位 108d2=.115739，FL_MB 首位 108f2=.014870，后续 `sort_basket` 分支 11762=.009506、1177c=.006798。事件可重叠且不证明临界路径；完整九事件各自前列见 JSON。

此前 CI 对照的 initial DTLB miss 数可能因 PTW miss queue 重试重复累计；PTW 内存请求、TLB 替换与 replay 数没有同比六倍增长，详见 mcf-translation-counter-notes.md。故 ST_TLB 高是明确观测，不足以证明容量失败是根因或可回收 CPI。候选验证按 LSU 内部队列/entries、DTLB 单轴和请求时序优先，区分固有地址链及可隐藏的翻译/缓存等待；L2/L3 总容量默认维持，MSHR/bank 可单轴，明确容量或预取污染证据时可用扩大容量作诊断。本次没有新增模拟。
