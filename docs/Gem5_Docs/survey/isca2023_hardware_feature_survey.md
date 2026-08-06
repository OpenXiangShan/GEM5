# ISCA 2023：面向 KMHv3/GEM5 的纯硬件性能特性筛选

> 来源：[ISCA 2023 proceedings DOI 10.1145/3579371](https://dl.acm.org/doi/proceedings/10.1145/3579371) 和 [DBLP 目录](https://dblp.org/db/conf/isca/isca2023.html)。

## 决策总表

|级别|feature|论文效果/开销|GEM5 建议|
|---|---|---|---|
|P0|EMISSARY|L2 instruction replacement，平均 +3.24%、最高 +23.7%，能耗平均 -2.1%|局部 cache policy，优先实现|
|P1|Orinoco|non-collapsible queue 的 ordered issue/unordered commit；IPC +14.8%，数 KB SRAM|后端范围大、正确性风险高，研究原型|
|P2|Imprecise Store Exceptions|消除 retired store 等待长异常检测的结构需求|需要 RISC-V/OS memory-model 协作，严格排除|

### EMISSARY：按前端停顿代价的 L2 I-cache replacement

[DOI 10.1145/3579371.3589097](https://doi.org/10.1145/3579371.3589097)。该 family 不追求所有 I-cache miss 最少，而保护会导致 decode starvation 的 line。论文在含 FDIP 的服务器系统中平均 +3.24%、最高 +23.7%，并报能耗下降。当前 `kmhv3.py` 的 L2 已有 `XSDRRIPRP`，所以应新加 instruction-line cost metadata/插入和 victim policy，明确区分 data/I-fill；保留 set/way、lookup latency 和 replacement port。

### Orinoco：可排序但不搬移的后端队列

[DOI 10.1145/3579371.3589046](https://doi.org/10.1145/3579371.3589046)。age matrix（bit-count encoding）、commit-dependency matrix 和 memory-disambiguation matrix 允许固定物理槽位的 non-collapsible queue 支持 ordered issue/unordered commit；论文以 8T SRAM 实现，开销为数 KB、IPC +14.8%。GEM5 不能只打乱 `seqNum`：需要保存 precise exception、load/store ordering、squash 和 commit 依赖。建议先实现 scheduler age matrix 而不打开 unordered commit，验证后再逐层放开。

## 跨会议实现参考（非 ISCA 2023 候选）

### Victima：MICRO 2023 的 cache-backed translation

[DOI 10.1145/3613424.3614276](https://doi.org/10.1145/3613424.3614276)。PTW cost predictor 只把昂贵 translation cluster 放进 L2，TLB-aware policy 决定何时保护这些 entry；不需应用或 OS 改动。论文 native/virtualized 提升 7.4%/28.7%，而且避免大 L2 TLB 的面积/功耗。当前 RISC-V 路径需要 TLB/PWC/L2 metadata 的确切所有权和填充/驱逐事件，A/B 必报 data-cache pollution 与 PTW traffic，不能只增大 TLB。

## 排除

- `Imprecise Store Exceptions` [DOI](https://doi.org/10.1145/3579371.3589087) 明确是硬件-软件协同，需要异常/OS 语义。
- `Utopia`、`Contiguitas` 以 OS 分配/映射为前提；`K-D Bonsai` 要 ISA extension。
- GPU、NPU、PIM、CXL/SSD/加速器和安全评估论文不属于当前 CPU 纯硬件 shortlist。

## 验证

ISCA 2023 候选按单独开关实现，默认关闭。EMISSARY 看 L1I/L2I miss、decode starvation；Orinoco 看 issue/commit width、queue occupancy、replay/squash。Victima 的验证口径保留在本节仅作跨届实施参考。使用同一 checkpoint、warmup/ROI，以最终 reset stats 比较。
