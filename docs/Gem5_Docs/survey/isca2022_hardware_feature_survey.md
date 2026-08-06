# ISCA 2022：面向 KMHv3/GEM5 的纯硬件性能特性筛选

> 书目来源：[ISCA 2022 DBLP](https://dblp.org/db/conf/isca/isca2022.html) 和 [ACM proceedings DOI 10.1145/3470496](https://dl.acm.org/doi/proceedings/10.1145/3470496)。本届严格筛选后，通用 CPU 的硬件-only 主候选较少；这是题材分布的结果，不以 accelerator/软件协同论文填充清单。

## 结论

|级别|feature|论文效果和开销|判断|
|---|---|---|---|
|P0|Register File Prefetching (RFP)|把 43.4% load 从 L1 预取到 RF；Tiger Lake-like +3.1%，放大 core +5.7%|最贴近 O3 load/RF path，但需精确 recovery/liveness|
|P2|tākō polymorphic cache|软件 callback 驱动近 cache dataflow engine；1.4-4.2×、约 +5% 多核面积|性能高但非 hardware-only，明确排除|
|P2|Tiny but mighty/MAPLE|NoC 接入异步 memory engine；全系统测得相对现有 HW 方法 1.72-1.82×|需要硬件-软件接口、编译，明确排除|

### P0：Register File Prefetching

论文：[DOI 10.1145/3470496.3527398](https://doi.org/10.1145/3470496.3527398)。RFP 使用 OoO scheduling pipeline 和空闲 L1/RF 带宽，在 load 实际需要前把数据从 L1 写入 RF。论文在 65 workload 中预取 43.4% load，Tiger Lake-like baseline +3.1%，up-scaled core +5.7%；与 value prediction 合用平均 +4.1%。

**GEM5 映射。** 在 `src/cpu/o3/lsq.*`、`iew.*`、physreg/scoreboard 中增加 load destination 的预取-ready 状态。正确性关键是：RFP 仅在 L1 hit 后填入对应 physical register 的 side buffer，consumer 只能在 value valid 后唤醒；squash、replay、store violation 和寄存器重命名必须使记录失效。硬件成本至少包括每个 in-flight load 的 `rf_prefetched/valid` 位、地址/版本关联、L1/RF read-write port 和 arbitration；模型不能免费增加 RF 端口。

**验证。** 先做 L1-hit 长依赖 microbenchmark，再跑 checkpoint；测 L1 hit latency、RFP issued/hit/late/drop、RF port conflict、wakeups、load-use stall、squash/replay 与 IPC。分别比 `off / RFP / value-pred / RFP+value-pred`，避免重复归因。

## 严格排除/不建议移植

|论文|原因|
|---|---|
|tākō|[DOI](https://doi.org/10.1145/3470496.3527379) 让 cache miss/eviction 触发软件 callback 并在 near-cache reconfigurable engine 上执行；软件接口是机制核心。|
|Tiny but mighty/MAPLE|[DOI](https://doi.org/10.1145/3470496.3527400) 需要硬件-软件异步访问接口和自动编译。|
|Free atomics|以新 atomic/fence 语义暴露给软件，非不改软件的硬件 feature。|
|Thermometer、Sibyl|分别依赖 profile 或 OS/data-placement policy。|
|PIM/GPU/accelerator/量子/安全论文|不落在当前通用 O3 CPU 软硬件不变的优化范围。|

## 当前树与成本闸门

`kmhv3.py` 是单核/少核 OoO 配置，RF/rename/LSQ 路径在 `src/cpu/o3/`。RFP 实施前须量化 int/fp RF port、load queue 和 writeback width；若实现仅把 L1 load response 提前交付而没有消耗可用端口/队列，则不是可信硬件模型。默认关闭，保留原统计与时序。
