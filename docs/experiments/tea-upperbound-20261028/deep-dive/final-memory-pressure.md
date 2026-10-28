# 最终12宽组合的缓存压力：代表点筛选

只读完整 CI `40648104599` / run1676 的 20M 正式末块，并与 baseline24 和 R4 四 load + 前端对应点对照；未做新实验。覆盖 mcf_12886、mcf_6753、libquantum_69175、astar_rivers_8610、omnetpp_18492，均为 0.3c 代表点，不是完整 benchmark 结论。原始计数器路径、描述与配置见 [final-memory-pressure.json](final-memory-pressure.json)。

实际最终配置：L1D 64 KiB、16 MSHR、每 MSHR 20 targets；L2 wrapper 32 MiB、四 slice 各 8 MiB/16 MSHR/20 targets、data/dir SRAM 各 2 banks；L3 64 MiB、64 MSHR/20 targets。内存总容量保持现状作为首选。

| 切片 | core cycles | L1 no-MSHR blocked cycles | L2 四 slice no-MSHR cycles 之和 | L2 MCP2 重试 | L2 目录 SRAM 重试 | L3 no-MSHR cycles |
|---|---:|---:|---:|---:|---:|---:|
| mcf_12886 | 17,075,174 | 47,902 | 310,935 | 1,712,795 | 1,982,976 | 0 |
| mcf_6753 | 15,869,153 | 15,757 | 29,123 | 579,377 | 816,930 | 0 |
| libquantum_69175 | 2,827,929 | 0 | 0 | 211 | 142 | 0 |
| astar_rivers_8610 | 4,019,343 | 0 | 0 | 3,554 | 5,512 | 0 |
| omnetpp_18492 | 8,222,150 | 593 | 21 | 75,917 | 116,640 | 0 |

四 slice 的周期可并发，不能把和当 CPU 独占停顿；MCP2 和目录是 L1 MSHR 请求检查进入主流水时的资源冲突事件，可重复且可重叠（`src/mem/cache/xs_l2/L2MainPipe.cc:159-187`），也不是节省周期。baseline→R4→R5 对 mcf_12886 的 L2 no-MSHR 汇总为 145,838→146,471→310,935，核心 cycles 18,613,623→18,018,570→17,075,174；更快和更高占用可同时存在，并不证明 MSHR 扩容会加速。omnet、astar、libquantum 的 no-MSHR 很少或为零，不支持先扩大 L3 MSHR 的通用收益主张。

## 预取器统计和限制

最终 mcf_12886 的 L1 parent `pfUseful=91,318`、`pfLate=1,458,346`、`pfUnused=4,085`；L2 parent 为 1,081,829 / 1,449,276 / 57,996。omnetpp L1 为 44,110 / 20,470 / 61,655，L2 为 62,694 / 969,315 / 158,471；astar L1 为 3,294 / 813 / 6,018；libquantum L1 为 620,569 / 28 / 59。分别对应 useful / late / unused；完整来源和其它计数见 JSON。

`pfLate` 定义为预取请求命中 cache、MSHR 或 WB 的计数之和（`src/mem/cache/prefetch/base.cc:300-312,368`），不是 demand 等预取的延迟；`pfUnused` 是带预取标记的块未被引用即驱逐（`:271-274`），不是已证明污染。父/子 prefetcher 的发出与 useful 统计范围可不同，不直接据这些数算准确率；没有逐请求竞争或反事实，不足以确定应关哪一预取源，也不足以断言需要扩大总容量。

## 受控候选和反证

**首选：最终组合上只将 L2 data SRAM banks 2→4**，保留 dir=2、MSHR 与容量。最小增量 CLI：`--param='system.l2_wrappers[0].data_sram_banks=4'`，叠加既有最终组合参数，不改其它配置。mcf 两点有实测 MCP2 重试，12886 尤其值得以 TEA 的关键链与 cache 自身 demand 服务时间验证；仍可能是重复重试或被并发隐藏，不能从 171 万次数推断收益。libquantum、astar 的 MCP2 仅 211/3554，可作预期弱影响保护点；omnet 的 75,917 提供独立路径。每组核验 config、40M 上限/20M reset、末块及 difftest，再比较 CPI、每 slice MCP2、目录冲突、需求服务和关键 PC。

若该轴没有改善，再按实际结果决定是否单测 dir banks 2→4，或对 mcf_12886 统一单测四 slice MSHR 16→32；不将两种轴同时修改，也不将计数变化相加。已知旧模型 data bank 筛选不能直接外推本新组合。`system.cpu.lsq0.loadReplayEvents::BankConflictReplay` 是 LSU 本地检查/重放类别（`src/cpu/o3/lsq.cc` 的冲突检查与 setReplay 路径），不能视为 L2 SRAM 实际 bank 冲突。现有统计无法区分缓存总容量不足与预取污染；只有后续有确切证据才将扩大容量用作诊断。
