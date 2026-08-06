# MICRO 2022：面向 KMHv3/GEM5 的纯硬件性能特性筛选

> 目录：[MICRO 2022 DBLP](https://dblp.org/db/conf/micro/micro2022.html)，论文出版入口为 IEEE/ACM [proceedings DOI 10.1109/MICRO56248.2022](https://doi.org/10.1109/MICRO56248.2022)。

## 推荐列表

|级别|论文/feature|效果与硬件成本证据|GEM5 可实现性|
|---|---|---|---|
|P0|Ballerino（Reconstructing OoO Issue Queue）|12 个级联/簇式 in-order IQ，达到 8-wide OoO 的相近性能，核心能效 +20%|改 issue queue/scheduler，需保留 wakeup、memory-dependence 和 phase 状态|
|P0|Pattern Merging Prefetcher (PMP)|相对 enhanced Bingo +2.6%，相对 Pythia +8.2%；pattern 存储分别少 30×/6×|新建 bounded pattern table，适合 L2 prefetch A/B|
|P1|Hermes|预测 off-chip load 并绕过 cache hierarchy latency；跨配置稳定加速，存储开销 modest|需 core 到 memory-controller 的 speculative request sideband，接口风险中等|
|P1|Berti / Page Size Aware Prefetching|Berti 为 2.55 KB、相对 IP-stride +8.5%；PPM 仅给 L1 MSHR 加 1 bit，最多使既有 spatial prefetcher +8.1%|可复用现有 prefetch queue，但应固定 table bits 与带宽预算|

### Ballerino：重构唤醒/选择

论文：[DOI 10.1109/MICRO56248.2022.00023](https://doi.org/10.1109/MICRO56248.2022.00023)。它把 readiness-based 与 dependence-based IQ 结合，使用 dispatch 过滤、memory-dependence steering 和共享 IQ 应对长延迟 miss。论文结论是 12 个 in-order IQ 可接近 8-wide OoO IQ，同时核心-wide energy efficiency +20%。GEM5 需在 `src/cpu/o3/issue_queue.*`/`inst_queue.*` 中增加固定级联队列、选择端口和 steering 状态；不能以“减少 wakeup 次数”作为零时延。A/B 记录 issue width、wakeup/select 次数、IQ occupancy、load dependence stall、IPC 和能耗代理。

### PMP：相似 pattern 合并

论文：[DOI 10.1109/MICRO56248.2022.00071](https://doi.org/10.1109/MICRO56248.2022.00071)。用 region 首次访问 offset 对 pattern 聚类，在训练阶段合并相似 footprint，发射阶段按访问频率选择目标。结果为 enhanced Bingo +2.6%、Pythia +8.2%，同时大幅降低表存储。GEM5 先在 `src/mem/cache/prefetch/` 实现固定 set/way、每项 tag+offset bitset+frequency；每次 train/issue O(1)，表满只在固定候选中淘汰。比较应固定总 bit budget，统计 coverage、accuracy、late、pollution、metadata hit 和 DRAM traffic。

### Hermes：绕过 cache hierarchy 的 off-chip load prediction

论文：[DOI 10.1109/MICRO56248.2022.00015](https://doi.org/10.1109/MICRO56248.2022.00015)。轻量 perceptron 使用 PC 序列、load byte offset 等特征预测是否会 off-chip；地址生成后直接向 memory controller 发起 speculative load，同时访问 cache，命中预测时隐藏 cache lookup latency。论文只给出“多配置稳定提升、modest storage”，没有可核对的统一 IPC/bit 数，应按未知成本处理。GEM5 需要固定 predictor entries、lookup latency、speculative queue 和错误请求取消路径；验证必须把 cache hit 误判、内存带宽占用和 load latency 分开。

### Berti/页大小感知预取

[Berti DOI 10.1109/MICRO56248.2022.00072](https://doi.org/10.1109/MICRO56248.2022.00072) 是按 load PC 选择 local delta 的 L1D prefetcher；公开摘要给出 **2.55 KB** storage、相对 IP-stride **+8.5%**、相对 IPCP **+3.5%**，并报告相对 IPCP 的 memory-hierarchy dynamic energy -33.6%。[Page Size Aware Cache Prefetching DOI 10.1109/MICRO56248.2022.00070](https://doi.org/10.1109/MICRO56248.2022.00070) 的 PPM 将 page-size 信息传给低层预取器，为 L1 MSHR 增加 **1 bit**，在 80 个 memory-intensive workload 上使所评估的既有 spatial prefetcher 单核提升 **2.1%--8.1%**。两者都是硬件 data prefetch 方向，但基线、cache level 和 page-size 假设不同，不能把数字横向合并。本仓库可把它们作为 P1 复现：限定 region/page bits、degree、队列和 MSHR，分别跑 no-prefetch、现有 SMS/BOP、候选预取器；任何收益必须由 demand MPKI、late/useful 比例和带宽统计解释。若只改变 page-size metadata 而没有硬件请求/带宽动作，不算 feature。

## 排除项与边界

- `Speculative Code Compaction`、`OCOLOS`、`Treebeard` 等依赖编译器/二进制变换。
- GPU/PIM/加密/持久内存和网络 accelerator 不属于当前 KMHv3 CPU 性能路径。
- `SwiftDir`、`Eager Memory Cryptography in Caches` 是安全/协议机制，不能用作裸 CPU 加速。

## 验证入口

核心入口为 `src/cpu/o3/inst_queue.*`、`src/cpu/o3/issue_queue.*`、`src/mem/cache/prefetch/` 和 `configs/common/PrefetcherConfig.py`。所有实验固定同一 checkpoint、warmup/ROI 和内存配置；比较 reset 后 IPC/simTicks，补充 IQ/wakeup 或 prefetch coverage/traffic counters，并做 queue/table/degree sweep 作为硬件开销敏感性分析。
