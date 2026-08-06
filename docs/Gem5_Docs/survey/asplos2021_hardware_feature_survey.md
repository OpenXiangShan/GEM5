# ASPLOS 2021 面向 KMHv3/GEM5 的硬件特性筛选

> 范围：[ASPLOS '21 proceedings](https://dl.acm.org/doi/proceedings/10.1145/3445814) 与 [DBLP 目录](https://dblp.org/db/conf/asplos/asplos2021.html)。本报告的“硬件-only”排除需要编译器、binary hint、OS allocator、运行时或专用 accelerator 的方案。

## 结论

|级别|论文/feature|论文效果与成本|当前 GEM5 判断|
|---|---|---|---|
|P1|BCD deduplication|LLC/主存 partial-line dedup；SPEC 平均 +2.7%，综合 compression ratio 1.94×，modest area|内存压缩+cache metadata，行为模型可做但不应低估压缩时延|
|P2|Voyager hierarchical neural prefetching|irregular SPEC/GAP 对 no-prefetch +41.6%；相对旧神经模型计算 15-20×低、存储 110-200×低|仍受训练/推理时延限制，适合作为高风险 research model|
|P3|DiAG|512 PE RISC-V 原型相对激进 OoO 1.18×、能效 1.63×|硬件-only，但几乎是重建 CPU，不是现有 KMHv3 的可控 feature|

### BCD：partial cache-line deduplication

[DOI 10.1145/3445814.3446722](https://doi.org/10.1145/3445814.3446722)。BCD 组合 base/difference compression 与多条 line 的 partial dedup，同时压缩 LLC 和主存；论文在 SPEC2017/DaCapo/TPC-DS/TPC-H 得到 1.94× 压缩比，SPEC 平均性能 +2.7%。硬件状态包括 compressed payload、base/difference pointer、reference count、allocation/free list 和解压端口；每个 access 的压缩判断和二次 metadata request 不能省略。

**GEM5 路径。** 先在最后级 cache/内存 controller 之间实现固定 compressor latency、有限 compressed-sector 容量和 writeback traffic，再看有效 capacity 是否真的改善 demand miss。当前单核 2 MiB L2 没有共享 LLC，不应直接照抄论文数字；实验需报告 compression ratio、metadata overhead、extra reads/writes、decompression stall、MPKI 与 IPC。

### Voyager：分层神经预取

[DOI 10.1145/3445814.3446752](https://doi.org/10.1145/3445814.3446752)。分别预测 page 和 offset 的地址关联，目标是不规则 access；论文以 no-prefetch 比较得到 IPC +41.6%，并称训练/推理规模已经比旧模型低，但仍明确尚不够快。GEM5 可以在 `src/mem/cache/prefetch/` 做定长 embedding/table、固定 inference latency 与更新带宽的模型；不可在热路径运行无限精度/无限上下文模型。先做 trace replay 的 accuracy/coverage，再接入 request queue。

### DiAG：数据流化通用 CPU（不建议近期实现）

[DOI 10.1145/3445814.3446703](https://doi.org/10.1145/3445814.3446703)。register lanes 在硬件中隐式构造 dataflow graph，不需要特殊语言或 compiler；论文的 RISC-V 512-PE 实现相对 aggressive OoO 有 1.18× 性能、1.63× 能效。代价是 PE array、lane register file、front-end/commit 全路径重构，远超一个 feature 的实现/验证边界。因此只作为长期基准，不写入当前 O3 局部计划。

## 排除

- `NOREBA` 是 compiler-informed；`PIBE` 需要 profile-guided binary 处理。
- `PTEMagnet`、vMitosis、KLOCs 等依赖 OS/物理内存策略。
- NIC/FPGA/PIM/SSD/量子/安全和各类 accelerator 不对当前 CPU 软件透明的性能链产生直接作用。

## 验证与成本纪律

BCD/Voyager 默认关闭且保留带宽、端口、metadata 和时延。固定 checkpoint 的 warmup/ROI 后，比较 reset stats；若性能变化不能由 cache capacity、prefetch useful/late、decompression 或 DRAM traffic 解释，则不接受为可实现的 GEM5 feature。
