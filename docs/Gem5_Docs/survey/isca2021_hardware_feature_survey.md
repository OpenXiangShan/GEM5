# ISCA 2021：面向 KMHv3/GEM5 的纯硬件性能特性筛选

> 来源：[ISCA 2021 DBLP 目录](https://dblp.org/db/conf/isca/isca2021.html) 及各论文 DOI。ISCA 为 ACM/IEEE 会议；ACM DL 若出现访问验证，以 DOI/DBLP 目录作为可复核书目入口。

## 优先级总表

|级别|论文/feature|论文效果与开销|当前 GEM5 映射|
|---|---|---|---|
|P0|ATP + SBFP TLB prefetch|SPEC 几何加速 +11.1%，page-walk memory refs -26%|RISC-V TLB/PTW 和小型 predictor/filter|
|P0|Entangling instruction prefetcher|40 KB，性能最高 +23%|IFetch/L1I/L2 prefetch queue，需严格带宽/污染建模|
|P1|Vector Runahead|间接访问 workload 1.79×|复杂 in-core runahead，适合分阶段预取原型|
|P1|PF-DRAM|平均性能 +8.6%、最高 +24.3%，memory power -35.3%，面积 <9%|仅在可改 DRAM timing model 时；不是 O3 局部改动|
|P2|Zero Inclusion Victim|消除 inclusive LLC 对 core cache 的强制驱逐，支持更大 mid-level cache|当前默认非同构共享 LLC 语境不完全匹配，作为多核缓存项目|

## 候选论文

### ATP + Sampling-Based Free TLB Prefetching

[DOI 10.1109/ISCA52012.2021.00016](https://doi.org/10.1109/ISCA52012.2021.00016)。ATP 按 phase 从三种低成本 TLB prefetcher 中选择，SBFP 从相同 page-table cache line 的相邻 PTE 挑选有用条目，不必额外完成 page walk。论文报告 Qualcomm/SPEC/GAP 速度提升 16.2%/11.1%/11.8%。实现状态是 predictor tables、usefulness sampling 和过滤器；GEM5 在 `src/arch/riscv/tlb.*` 与 walker 增加固定队列/端口，A/B 要报告 PTW request 数、TLB MPKI、MSHR pressure、coverage/late/unused。

### Entangling instruction prefetcher

[DOI 10.1109/ISCA52012.2021.00017](https://doi.org/10.1109/ISCA52012.2021.00017)。学习“哪一条已取指令应触发未来 instruction 的预取”，显式用 miss latency 选 trigger，兼顾 timeliness/coverage/accuracy。论文给出 40 KB 状态、性能最高 +23%。GEM5 不能简单把它写成无延迟 PC correlation：每项需有 tag/target/valid/confidence，触发后经 `Queued` prefetch request、L1I/L2 MSHR 和 DRAM 带宽；保留 request priority 和错误路径污染统计。

### Vector Runahead

[DOI 10.1109/ISCA52012.2021.00024](https://doi.org/10.1109/ISCA52012.2021.00024)。在 runahead 中暂缓 cache miss，并跨 loop iteration 把间接链标量操作重排成向量访问，提升 MLP；论文在 memory-latency-bound indirect workload 报 1.79×。硬件需要 runahead checkpoint、vectorized temporary state、地址生成与 cache request ports，远高于普通 prefetcher。GEM5 建议先实现只读 one-hop load-chain engine，再逐步加入多链和 vector packing；统计 MLP、prefetch timeliness、recovery/squash 和 bandwidth。

### PF-DRAM

[DOI 10.1109/ISCA52012.2021.00019](https://doi.org/10.1109/ISCA52012.2021.00019)。修改 subarray precharge/equalizer/sense amplifier，复用前一 activation 的 bitline charge，memory controller 仅少量改动。论文 8 GB SPEC/PARSEC 系统平均性能 +8.6%、memory power -35.3%、面积 <9%。它满足硬件-only，但在 GEM5 必须先在 DRAMsim3/Ramulator timing 中实现 row-state 和 ACT/PRE 约束，不能只减少固定 memory latency。

### Zero Inclusion Victim

[DOI 10.1109/ISCA52012.2021.00015](https://doi.org/10.1109/ISCA52012.2021.00015)。当 set 内候选会造成 inclusion victim 时允许全局 victim selection，保持 LLC inclusion 却不因 LLC eviction 强制驱逐私有缓存。论文没有通用单核 IPC 数字，而是显示接近 non-inclusive LLC、且随 mid-level cache 增大优势提升。成本是 directory/全局 victim 搜索和 coherence 审核；现有单核 KMHv3 不应优先实现，只有多核 inclusive LLC 配置才立项。

## 排除项

- `Ripple` 是 profile-guided I-cache replacement，需要离线 profile。
- `Execution Dependence Extension`、`Unlimited Vector Extension` 等需要 ISA/编译器支持。
- PIM、专用加速器、云资源调度、可靠性/安全机制没有直接对应当前 CPU 性能链。

## 统一验证

固定 checkpoint、软件二进制与 ROI，只改变硬件开关或 bit budget。预取项至少报 IPC、L1I/TLB MPKI、PTW/DRAM traffic、MSHR occupancy、useful/late/unused；内存/多核项先完成 timing/coherence 单元检查，再进行应用 A/B。
