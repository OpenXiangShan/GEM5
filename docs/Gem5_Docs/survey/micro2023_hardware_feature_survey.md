# MICRO 2023：面向 KMHv3/GEM5 的纯硬件性能特性筛选

> 来源：[MICRO 2023 proceedings DOI 10.1145/3613424](https://dl.acm.org/doi/proceedings/10.1145/3613424) 与 [DBLP 目录](https://dblp.org/db/conf/micro/micro2023.html)。

## 推荐列表

|级别|feature|论文效果/成本|GEM5 判断|
|---|---|---|---|
|P0|DVR（Decoupled Vector Runahead）|1139 B；相对 5-wide OoO 2.4×、相对 Vector Runahead 2×|独立轻量 in-order 硬件子线程，适合新增可控 prefetch engine|
|P1|CLIP|关键 load 预测准确率 >93%；64-core/8-channel 对 Berti 有效性 +24%/ +9%；1.56 KB/core|当前单核需先验证 bandwidth/criticality 统计，缩放后再测多核|
|P1|Micro-Armed Bandit|prefetch 相对 Bingo/MLOP +2.6%/+2.3%，仅 100 B；SMT fetch +2.2%|极小控制器，可作为多预取器 admission/degree manager|
|P2|Victima|利用 L2 保存高 PTW 成本页的 TLB entry；native/virtualized +7.4%/+28.7%，面积/功耗 +0.04%/+0.08%|需 L2-TLB metadata 和 replacement 协同，硬件-only 但系统验证较重|

## 论文摘要与实现合同

### DVR

[DOI 10.1145/3613424.3614255](https://doi.org/10.1145/3613424.3614255)。DVR 运行与主线程解耦的 speculative in-order subthread，运行时推断 loop bound、识别 stride load 并向量化间接链，动态调节向量化 degree。论文报告 1139 B 硬件开销和 2.4×/2× 性能。GEM5 可在 `src/mem/cache/prefetch/` 新增 `DvrPrefetcher`，但必须建模启动延迟、向量 lane 数、L1 MSHR/带宽竞争、分支 divergence 和 throttle；不能把每个间接链当作无延迟 oracle。

### CLIP

[DOI 10.1145/3613424.3614245](https://doi.org/10.1145/3613424.3614245)。CLIP 预测在已有 prefetchers 下仍会造成 stall 的 critical loads，并过滤其已能被准确预取的请求。论文显示 bandwidth-constrained 64-core 系统中 Berti slowdown 可被扭转，硬件状态 1.56 KB/core。GEM5 第一阶段只做 criticality predictor/admission，不改 Berti 训练；统计 predictor hit、demand/prefetch overlap、MSHR/DRAM queue 和 bandwidth。

### Micro-Armed Bandit

[DOI 10.1145/3613424.3623780](https://doi.org/10.1145/3613424.3623780)。利用时间窗口内 action space 的 temporal homogeneity，用 bandit 代替复杂 RL 表；100 B 状态即可管理 prefetch，且可复用到 SMT instruction fetch。GEM5 可放在 L2 composite worker 层；参数化 arms、epoch、reward 和 degree，保持队列 backpressure。必须比较 no-manager、固定 degree、bandit 三组。

### Victima

[DOI 10.1145/3613424.3614276](https://doi.org/10.1145/3613424.3614276)。PTW cost predictor 选择高代价页，将 L2 block 重用为 TLB-entry backing，并用 TLB-aware replacement 减少 PTW。论文完全 software-transparent，平均 native +7.4%、virtualized +28.7%，面积/功耗 +0.04%/+0.08%。GEM5 需在 `src/arch/riscv/tlb.*`、L2 tag metadata 和 PTW 中加 bounded entry type，统计 PTW latency、TLB hit、data-cache pollution 和 entry eviction。

## 跨会议实现参考（非 MICRO 2023 候选）

### EMISSARY：ISCA 2023 的 L2 instruction replacement

[DOI 10.1145/3579371.3589097](https://doi.org/10.1145/3579371.3589097)。针对 instruction L2，用 decode starvation 代价而非 miss 数指导 replacement，保护导致高代价前端停顿的 line。论文服务器 workload 平均 +3.24%、最高 +23.7%。在 GEM5 的 `XSDRRIPRP` 或独立 policy 中增加 miss-cost/occupation bits；要求记录 L1I miss、decode starvation、line fill/evict 和 replacement decision，每次更新 O(1)。

## 排除项

`Clockhands` 需要 rename-free ISA；`Ignite` 记录并恢复 serverless invocation 的 CFG metadata；`Imprecise Store Exceptions` 需要 ISA/OS 语义；`Utopia`/`Mosaic Pages` 涉及 OS huge-page/地址映射；GPU、PIM、NPU 和安全论文不纳入硬件-only CPU shortlist。

## 验证

从 `kmhv3.py` 固定核心数、L1/L2、DRAM 和 checkpoint；warmup 后 reset stats。DVR/CLIP/Micro-Armed Bandit 重点看 prefetch coverage/late/unused、带宽和 MSHR；Victima 看 TLB MPKI/PTW cycles。EMISSARY 的 instruction MPKI/decode-starvation 口径保留为跨届参考。扩容表项只能在性能收益与存储 bit、端口和时序变化相互解释时接受。
