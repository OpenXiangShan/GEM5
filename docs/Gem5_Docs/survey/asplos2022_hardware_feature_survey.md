# ASPLOS 2022 面向 KMHv3/GEM5 的硬件特性筛选

> 来源：[ASPLOS '22 proceedings](https://dl.acm.org/doi/proceedings/10.1145/3503222) 和 [DBLP 目录](https://dblp.org/db/conf/asplos/asplos2022.html)。严格应用“只改硬件、二进制/OS 不变”后，本届没有足以列为 P0 的通用 KMHv3 feature；以下保留接近候选，明确标出缺失的软件条件。

## 筛选列表

|状态|论文|feature、论文效果与开销|结论|
|---|---|---|---|
|条件 P1|CRISP|关键 slice 优先执行以隐藏 irregular load/branch latency；memory-bound 平均 +8.4%、最高 +38%，硬件修改小|需要软件分类、slice 提取和新 instruction prefix，严格不满足|
|条件 P2|Every walk's a hit|flatten page table + cache priority；native +9.2%，virtualized +14.0%|需要 kernel page-table layout，硬件部分可拆出但论文机制非硬件-only|
|排除|Nested elastic cuckoo page tables|virtualized translation 平均 1.19-1.24×|guest/host page table 变更与迁移路径是必要条件|
|排除|Pinned loads / SparseCore / TaskStream|依赖 ISA、编译器或编程模型|不适合当前二进制不变的 GEM5 A/B|

### CRISP：关键 slice prefetch（条件候选）

[DOI 10.1145/3503222.3507745](https://doi.org/10.1145/3503222.3507745)。CRISP 将 delinquent load/branch slice 放到优先级更高的 scheduler 路径；论文报告 memory-latency-bound 应用平均 IPC +8.4%、最高 +38%。它有意把 memory access classification、slice extraction 和 priority analysis 放在软件，硬件只支持 instruction prefix。因此即使 issue queue 增加 priority bit 很容易，也不能声称实现了 CRISP。

可做的纯硬件衍生实验是在线 critical-load predictor：只使用 PC、load miss/replay、ROB age、consumer stall 建表，不读取 binary hint。这个实验不继承论文 +8.4% 数字，需要单独给 predictor storage、lookup latency、误判和 issue-starvation stats。

### Every walk's a hit（条件候选）

[DOI 10.1145/3503222.3507718](https://doi.org/10.1145/3503222.3507718)。page-table flattening 减少层数，cache replacement 在高 TLB-miss phase 保护 PTE；论文 native +9.2%，virtualized +14.0%，并报告 cache/DRAM dynamic energy 降低。但 flattening 需要 kernel allocation/self-reference 处理。纯硬件可独立复用的是“TLB-pressure-aware PTE protection”：在 L2 replacement 中对 PTE line 做有限状态 priority，保留 ordinary data pollution、PTW request 和 data MPKI 统计；它只是消融，不是论文全方案。

### Nested elastic cuckoo page tables（排除）

[DOI 10.1145/3503222.3507720](https://doi.org/10.1145/3503222.3507720)。它把 guest/host 都改成 HPT，在并行访问约束下把最坏 24 步 nested translation 降至 3 步；平均 1.19×/1.24×。page-table 结构、migration 与虚拟化环境不可由当前硬件单独替代，因此不纳入。

## 当前建议

不要为了凑数改写 ASPLOS 2022 的软件协同论文。近期应从 ISCA 2021 ATP/SBFP、MICRO 2022 PMP 或 HPCA 2022 cache-level prediction 启动；若未来允许 OS/ISA 改动，再把 CRISP/Every walk's a hit 放入独立 cross-stack 项目。所有纯硬件衍生实验仍需固定 entries、端口、更新宽度和 queue backpressure，并报告 ROI reset stats。
