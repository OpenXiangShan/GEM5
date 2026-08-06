# HPCA 2022 面向 KMHv3/GEM5 的硬件特性筛选

> 来源：[HPCA 2022 DBLP 目录](https://dblp.org/db/conf/hpca/hpca2022.html) 与论文 DOI。只保留软件不变时可由 core/cache/memory 硬件状态独立驱动的候选。

## 决策总表

|级别|feature|论文效果和成本|GEM5 优先级|
|---|---|---|---|
|P0|Cache Level Prediction|平均 access latency -20%，性能 +10.3%（常规基线）/+6.1%（boosted）|load 发射/LSQ 与多级 cache 并行请求，直接针对 O3 stall|
|P0|Mockingjay|相对 LRU：单核 +5.7%，无 prefetch 多核 +15.2%，有 prefetch CVP +20.1%|PC reuse-distance predictor，适合 replacement-policy A/B|
|P1|GBDI|compression ratio 2.3×，带宽 1.5×，SPEC17 中高带宽 workload +10%|cache/memory compression 项目，需显式 compressor latency|
|P1|Reliability-Aware Runahead|memory-intensive +33.5%（同时 MTTF 4.8×）|纯硬件但 core recovery 工作量大|
|P2|AVA vector RF|8 KB VRF + issue queue，短向量 2×、面积 -50% 对长向量基线|需 RVV/vector model，非当前 scalar P0|

### Cache Level Prediction

[DOI 10.1109/HPCA53966.2022.00054](https://doi.org/10.1109/HPCA53966.2022.00054)。预测 load 将由哪一层满足，在 L1 miss 后提前启动目标层 access；论文平均 latency -20%、性能 +10.3%，相对强化基线仍 +6.1%。GEM5 需让预测的 L2/LLC/memory request 与普通 L1 lookup 并行，并解决 duplicate response、MSHR ownership、store/squash；预测错误必须计入额外 traffic/port contention。参数为 per-PC table、confidence、target-level latency、最大 outstanding bypass 数；验证记录 correct/wrong、saved cycles、extra requests、L2/DRAM queue 和 IPC。

### Mockingjay

[DOI 10.1109/HPCA53966.2022.00048](https://doi.org/10.1109/HPCA53966.2022.00048)。PC predictor 使用多类 reuse-distance 而非 cache-friendly 二元分类，接近 Belady MIN 的 victim 顺序。论文无 prefetch 的 100 个多核 mixes 相对 LRU +15.2%（SHiP +7.6%、Hawkeye +12.9%），单核 +5.7%。GEM5 作为 L2 `ReplacementPolicy` 时需固定 predictor entries/tag/counter 位宽、lookup/update latency；当前单核 2 MiB L2 与论文 LLC 不同，先测 L2 iso-capacity，后测多核/共享层。

### GBDI

[DOI 10.1109/HPCA53966.2022.00085](https://doi.org/10.1109/HPCA53966.2022.00085)。以跨 block global base 做 BDI，background clustering 选择 bases；论文给 2.3× compression、1.5× bandwidth、平均 +10% performance，并称 accelerator latency/area 低。模型必须明确 base-table size、compressed format、compression/decompression ports、writeback expansion 和 DRAM transfer bytes；不要把主存容量无代价扩大。

### Reliability-Aware Runahead

[DOI 10.1109/HPCA53966.2022.00062](https://doi.org/10.1109/HPCA53966.2022.00062)。早启动 runahead 并使等待 memory 时的易错微状态不可见；论文 memory-intensive 平均 +33.5%、MTTF 4.8×。性能因果仍是 latency hiding，适合先忽略 reliability 指标、实现 precise runahead prefetch；但 checkpoint、rename/ROB/LSQ recovery 和 memory dependence 是高风险，放在 P1。

### AVA

[DOI 10.1109/HPCA53966.2022.00063](https://doi.org/10.1109/HPCA53966.2022.00063)。8 KB VRF、可重配 MVL 和新 IQ；短向量配置相对默认 2×，对长 vector 保持竞争力并省 50% area。它需要完整向量执行/寄存器建模，当前 scalar KMHv3 只有在 RVV 项目范围内才有意义。

## 排除与验证

`CRISP`、`tākō` 等需要 software/ISA；GPU/PIM/NoC/量子/accelerator 不纳入。P0 实现默认关闭、每请求 O(1) 或固定 way 扫描。固定 checkpoint 的 reset ROI 对比不仅报 IPC，还报 predictor hit、queue/MSHR、MPKI、带宽、metadata bit 数和 port 争用。
