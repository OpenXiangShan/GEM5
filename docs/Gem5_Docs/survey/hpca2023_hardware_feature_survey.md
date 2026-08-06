# HPCA 2023 面向 KMHv3/GEM5 的硬件特性筛选

> 来源：[HPCA 2023 DBLP 目录](https://dblp.org/db/conf/hpca/hpca2023.html) 与各 DOI 摘要。结果按“当前 CPU 软件不改 + 有限硬件状态/端口 + 可做可信 GEM5 A/B”排序。

## 推荐总表

|级别|feature|论文效果/开销|当前 GEM5 判断|
|---|---|---|---|
|P0|ACIC|admission-controlled I-cache，平均 1.0223×|小 i-filter+temporal predictor，前端局部 feature|
|P0|BTB-X|同存储下比 conventional 跟踪 2.24× branch、比 PDede 1.3×|BTB target offset 编码，当前 BPU 最适配|
|P1|CARE|4/8/16 核相对 LRU +10.3%/+13.0%/+17.1%|并发感知 replacement；单核先做可观测性，重点多核|
|P1|Speculative Register Reclamation|RF 减半仍 +1.05×，功耗 -26%；扩展其他结构可 +1.14×|rename/free-list 生命周期高风险但性能链明确|
|P2|Victima-like ME-HPT|相对 radix HPT 1.23×，但需页表布局变更|非硬件-only，作为排除/跨栈参考|

### ACIC：admission-controlled I-cache

[DOI 10.1109/HPCA56546.2023.10071033](https://doi.org/10.1109/HPCA56546.2023.10071033)。小型 i-Filter 先分离 spatial 与 temporal access，再由 temporal predictor 判断 burst 后是否还会重用；论文平均 1.0223×，填补 LRU 到 OPT 差距的一半以上。实现落点为 L1I admission/bypass 或 L2 instruction insertion；状态为 filter tag、temporal counter、有限 update port。验证不仅看 I-cache MPKI，还要看被 bypass line 的后续 reuse、front-end stalls、prefetch interaction 和 filter false decision。

### BTB-X：target offset 分级存储

[DOI 10.1109/HPCA56546.2023.10070938](https://doi.org/10.1109/HPCA56546.2023.10070938)。观察到多数 branch offset 很短，将 set-associative ways 配置为不同 offset bit range；同存储下可追踪 conventional BTB 的 2.24×、PDede 的 1.3× 分支。`src/cpu/pred/btb/` 已有 block/target 元数据，适合先实现 fixed range ways、overflow path 和 decode latency。必须统计 short/long offset 分布、overflow/miss、bank conflict、实际 fetch bubble；不以理论 entry 数代替 IPC。

### CARE：并发感知 cache management

[DOI 10.1109/HPCA56546.2023.10071125](https://doi.org/10.1109/HPCA56546.2023.10071125)。用 pure miss contribution 衡量 outstanding miss 的有效代价，再动态调整 cache management；论文 4/8/16 核比 LRU +10.3%/+13.0%/+17.1%。状态包括 miss concurrency/PMC estimates、victim metadata 和 epoch policy。当前单核的 MLP 也可做，但不应把多核收益外推；需要建立 shared-cache/多请求者配置并统计 outstanding misses、per-core slowdown、miss overlap 与 replacement reason。

### Speculative Register Reclamation

[DOI 10.1109/HPCA56546.2023.10071122](https://doi.org/10.1109/HPCA56546.2023.10071122)。在 loop 重定义时推测提前释放 PReg，只为跨 iteration 实际使用的旧寄存器保留映射。论文 RF 减半仍有 1.05×、核心结构功耗 -26%，把节约资源给其他结构后可达 1.14×。GEM5 不能直接提前 `tryFreePReg()`：需为 rename source 建 use/redefine tracking，squash/exception/replay 精确撤销，release width 受限；统计 free-list pressure、early release/cancel、use-after-release guard 和 rename stalls。

## 条件项和排除

- Hybrid NVM LLC compression-aware insertion [DOI](https://doi.org/10.1109/HPCA56546.2023.10070968) 接近 SRAM 性能并将 lifetime 提升 17×，但需要 NVM LLC；当前 DRAMsim3 DDR4 路径仅作平台项目。
- Baryon [DOI](https://doi.org/10.1109/HPCA56546.2023.10071115) 可软件透明地管理 hybrid memory，平均 1.27×，但需要快/慢内存 tier。
- ME-HPT [DOI](https://doi.org/10.1109/HPCA56546.2023.10071061) 需 hashed page-table/allocator 变更，故不列硬件-only。

## 验证

ACIC/BTB-X 先做单核 checkpoint，CARE 再做多核；SpecReg 先跑 squash/exception correctness。每项固定存储 bit、端口和 lookup latency，报告最终 ROI IPC 与上述对应 causality stats；默认关闭以保持现有行为。
