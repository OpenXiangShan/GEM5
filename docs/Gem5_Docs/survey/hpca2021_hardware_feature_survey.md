# HPCA 2021 面向 KMHv3/GEM5 的硬件特性筛选

> 书目来源：[HPCA 2021 DBLP 目录](https://dblp.org/db/conf/hpca/hpca2021.html) 与 IEEE DOI。HPCA 由 IEEE 出版，ACM DL 不一定承载全文；本报告保留可交叉核对的 DOI、题名和摘要证据。

## 推荐列表

|级别|feature|论文效果/成本|GEM5 适配|
|---|---|---|---|
|P0|Dead Page + Dead Block Predictors|11 KB；14 memory-intensive workload IPC 平均 +8.3%|TLB/L2 death predictor，状态小、可局部 A/B|
|P1|RLR cache replacement|2 MB LLC 16.75 KB、8 MB LLC 67 KB；相对 LRU 单核 +3.25%、4 核 +4.86%|复用 L2 replacement，需适配当前无共享 LLC 的差异|
|P2|BlockHammer|低成本 RowHammer blacklist/调度|安全优先，性能效果依赖有 RowHammer mitigation 的基线|

### Dead Page and Dead Block Predictors

[DOI 10.1109/HPCA51647.2021.00050](https://doi.org/10.1109/HPCA51647.2021.00050)。论文先预测 last-level TLB 中 dead-on-arrival page，再用 page 信息驱动 LLC dead-block predictor；总 storage 11 KB，在 14 个 memory-intensive workload 上 IPC 平均 +8.3%。GEM5 可用 RISC-V TLB/last-level cache 的 fill/hit/evict 事件建立固定 PC/page signature 表；需要区分“predict dead 后 bypass”和“直接增大 cache”，并统计 false-dead、lost reuse、TLB/L2 occupancy 和 miss traffic。

当前 KMHv3 L2 是每核 2 MiB，且 current tree 没有论文中的共享 LLC/LLT 组合；建议先拆成 **L2 dead-block predictor** 和 **last-level TLB admission** 两个消融，固定 11 KB 等效 bit budget。若没有 long-lived L2/LLT 压力，论文收益不应外推。

### Reinforcement Learned Replacement (RLR)

[DOI 10.1109/HPCA51647.2021.00033](https://doi.org/10.1109/HPCA51647.2021.00033)。RL 仅离线用于发现特征，最终 RLR 是不需要 PC sideband 的硬件 policy；论文相对 LRU 单核/4 核 +3.25%/+4.86%，并给出 2 MiB/8 MiB LLC 的 16.75/67 KB 成本。该特点适合当前 `XSDRRIPRP`：新 policy 只读取 replacement-local reuse/age state，不把训练期 RL 放入模拟热路径。A/B 应与 LRU/SRRIP/DRRIP 做 iso-storage、iso-latency 比较，记录 insertion/victim 原因和 metadata bit 数。

### 边界与验证

`GreenDIMM`、`HoPP`、disaggregated/FAM、NDP/PIM/accelerator、持久内存和 OS resource management 不满足当前 CPU 硬件-only。RLR/Dead predictor 默认关闭，固定 checkpoint+ROI，检验 P95 miss latency、MPKI、eviction reuse、table full/drop 和 IPC；不能因为 predictor 表更大便与小基线作不公平比较。
