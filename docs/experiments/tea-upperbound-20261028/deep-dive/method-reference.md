# 旧 TEA dashboard 的证据链与当前复用方法

本次只读 `visualizations/baseline/dashboard.html`、直接内嵌数据和关联报告，未运行模拟、未重新核算原始 stats。下述量化结果是旧报告声称，除页面结构之外未独立重算。dashboard 是完全离线单文件，未引用外部 src/href；内嵌 `DATA` 包含 55 输入、8,835 热点、1,083 函数及切片矩阵，页面提供 workload、事件、PC、指令上下文、切片和 CSV 导出。Top 40 多指标并集不是全部 PC；完整数据在 `data/detail/all-pc-symbolized.csv.gz`，上下文在 `data/detail/assembly-context.json`（`reports/baseline/detailed-report.md:11-19,260-267`）。

```mermaid
flowchart LR
  A[正式窗口 TEA 状态与热点 PC] --> B[匹配 ELF hash / 反汇编 / 语义候选]
  B --> C[数据依赖或控制流假设]
  C --> D[缓存服务 / replay / 分支与供给等正交计数]
  D --> E[单轴配对与反例]
  E --> F[代表切片扩展和完整加权验证]
```

## 采样口径及版本边界

旧 dashboard 是 CI39234318080、`fdbb5057`、SPEC06 GCC16 RVA23 **1.0c**、1094 点、20M ROI、每周期采样（`reports/baseline/tea-performance-report.md:5,23-30`）。旧 TEA 有 8 个事件位 DR_L1/DR_TLB/DR_SQ/FL_MB/FL_EX/FL_MO/ST_L1/ST_TLB；修复版 `2647500974` 增加第九位 ST_LLC，修正停顿目标与融合 load 归因，见 `reports/final/tea-before-after-performance.md:7-20,540-542`。当前 515/365 的相同 checkpoint 同版本重采后才能作端点 ΔCPI；旧 dashboard 数字不能与新采样器直接相减，也不能把旧 1.0c 加权结果当当前 0.3c。先核对当前采样器提交与配置和五项 guest 计数、状态/CSV/特殊 PSV 守恒。

四状态全局互斥，Computing 仅表示有提交；事件可重叠不能相加，PC total 混合状态不是纯 stall；旧 CSV 不含 PC×状态联合分布。旧页面周期占比的分母是输入加权测量周期，做前后比较时统一用每正式提交指令的 CPI；累计 BPU PC 计数含预热，不能与 ROI TEA 相除估算逐错罚时（`reports/baseline/tea-performance-report.md:43-45,67,84-88,102,125`）。ELF 无法定位和 TEA unresolved 是不同缺口；已验证 ELF 和 rootfs hash 不等于 DWARF 精确源码行。

## 案例一：mcf 地址依赖 → 检验预取干扰

- **PC/源码**：`0x108d2 lw t4,24(a5)`、`0x108da ld t6,8(a5)`→`0x108e4 ld t6,0(t6)`；精确 ELF 为 `main` 内联汇编，`arc->ident` 和 `tail/head->potential` 是 reduced-cost 源码语义候选，无精确 DWARF 行。汇编可见地址依赖，首个 `lw` 提供控制判断；不可表述为 `lw` 的值直接产生 `ld` 地址。见 `reports/baseline/instruction-dossiers.md:7-35,38-62`、`detailed-report.md:31-43`、`evidence/source-candidates.txt`。
- **旧 TEA 定位**：mcf Stalled 76.91%；两 PC 输入权重 18.07%/15.04%，在 mcf_12886 各占 45.62%/34.00%；ST_L1 与 TLB 可重叠，ST_L1 不说明 L2/L3/DRAM。输入权重与单切片份额分母不同。
- **正交计数及单轴**：关闭 CDP 后 mcf_12886 周期 29,801,583→23,500,166，L2 预取发出 7,304,625→3,927,823，BOP useful 711,782→1,150,444，L1 demand 平均 miss latency 31631→15129 tick，branch miss 8824→8973（`reports/experiments/gem5-architecture-experiments.md:74-86`，原记录 `evidence/architecture-cdp-formal.json`）。支持该配置下预取流量与服务变化相关，尚不能拆分污染、带宽和队列竞争；gcc 反例需求 miss latency 上升且 IPC 回退。
- **下一步**：当前 0.3c 上先用同采样器 baseline/目标对关键地址链和自身 cache miss/latency、TLB/replay、预取来源重采；若证据指向拥塞，固定其余参数单独调整预取或有限资源，再保留正负切片并按完整覆盖计分。不要因热 PC 直接断言加大 load 端口能加速依赖链。

## 案例二：astar 分支 → 区分方向质量与供给时序

- **PC/源码**：biglakes `0x1db8e bne` 比较两条 `lhu` 所得值，语义候选 `waymap[...].fillnum == fillnum`；`0x1e516 bge` 为对象数据控制的循环边界。ELF 符号分别在 `way2obj::createway` 与 `regwayobj::createway`，无精确 DWARF 行（`reports/baseline/instruction-dossiers.md:255-310`、`detailed-report.md:65-74`）。
- **旧 TEA**：两个 PC 的 FL_MB 分别 2.20%/2.16% 全周期；biglakes 前五分支覆盖 42.99% FL_MB，gobmk 同指标仅 8.57%。这是优先 trace 排序，不证明某 PC 天然难预测；逐 PC BPU 累计含预热。
- **正交计数/反证**：旧 CDP-on 代表点 `astar_biglakes_11064` 的 PC threshold 对照，SC 纠正/破坏 TAGE 为 87303/56465→102991/62744，覆盖气泡 3788710→3653054，IPC +0.6023%；关闭 MicroTAGE 尽管最终误预测减少，覆盖气泡增加 408853，IPC −0.3674%（`reports/experiments/gem5-architecture-iteration2.md:53-72`、`evidence/iteration2-screen-mechanisms.json`）。不同 baseline、不同时间与非独占计数不能混合相加。
- **下一步**：对当前受益与回退 slice 分开采集分支 outcome/context、预测来源、恢复/override 与实际 target 供给，并分别验证单轴；不要仅凭少量热点或误预测率就判定扩表有效，更不能把 predictor 的覆盖气泡作为 CPU 独占周期。

## 案例三：lbm 除法热点 → trace 排除直觉方案

- **PC/源码**：`0x1056e fdiv.d fs5,ft9,fa5` 使用常数 1.0，`0x105de fmul.d` 使用结果；`lbm.c` 先累加 rho 后的速度归一化是语义候选，不是精确 DWARF 对应（`reports/baseline/detailed-report.md:93-101`、`evidence/source-candidates.txt`）。
- **旧 TEA**：PC 占 9.71% 总归因且 PSV=0；lbm 另有 20.56% unresolved。两者不能互相补足，也不能将 PSV=0 等同无执行依赖。
- **正交 trace 与反证**：`lbm_6382/4863` 的已提交目标各 99/100 个实例，都是 normal 12-cycle；源就绪中位 46/45 cycles，ready→select 为 1 tick，bypass→消费者选择中位 0 cycle。除数来自 `0x108ce fadd` 累加链，1606/1656 对相邻 fadd 的 AtFU 中位间隔为 2 cycles（`reports/experiments/gem5-architecture-iteration2.md:158-175`、`evidence/iteration2-fdiv/analysis.json`）。短 trace 支持依赖链检查，未发现额外除法端口等待/唤醒空隙；不是全程序关键路径证明。
- **下一步**：在当前正式 ROI 先复查动态生产者、源就绪到选择、真实除法类别及取样覆盖，再单轴验证依赖推进方案；不要仅凭旧 PC 占比缩短 FDIV latency 或增加 FU。

## 当前交付形式

主报告保留方向、验证结论和风险；独立 Markdown 为每个候选列输入/窗口/版本、PC与汇编、可证源码与候选源码、全局状态/事件及 unresolved、缓存/预测/队列正交计数、单轴对照和反例。用 Mermaid 描绘依赖链，图用现有 PNG/PDF 或常规绘图产物；不新建 HTML。只有正式同版本配对及完整加权验证后才称完整收益。
