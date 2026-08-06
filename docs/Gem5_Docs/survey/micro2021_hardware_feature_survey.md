# MICRO 2021：面向 KMHv3/GEM5 的纯硬件性能特性筛选

> 范围：MICRO '21 proceedings（[ACM 入口](https://dl.acm.org/doi/proceedings/10.1145/3466752)，[DBLP 目录](https://dblp.org/db/conf/micro/micro2021.html)）。先对目录题名全量初筛，再用论文摘要核对效果和开销。百分比是论文平台结果，不是 GEM5 承诺。ATP/SBFP 和 Voyager 只在文末作为跨会议实现参考，不属于本届候选。

## 结论与排序

|级别|论文/feature|机制、论文结果与开销|GEM5 判断|
|---|---|---|---|
|P0|Pythia|在线 bandit/RL 预取；单核相对 MLOP/Bingo +3.4%/+3.8%，12 核 +7.7%/+9.6%，面积 +1.03%|当前 L2 prefetcher/队列接口可复用，先做有界表和带宽反馈|
|P0|PDede|BTB 分区、目标去重、delta 编码；BTB miss 平均 -54.7%，IPC 平均 +14.4%（最高 76%）|直接落在 `btb_tage`/BTB entry，容量收益明显|

## 候选摘要

### Pythia：硬件 RL 预取

论文：[DOI 10.1145/3466752.3480114](https://doi.org/10.1145/3466752.3480114)。每个 demand 观察 PC、地址/stride 和带宽反馈，按 reward 选择预取动作；硬件 synthesis 报告面积 +1.03%，不需软件改动。GEM5 可在 `src/mem/cache/prefetch/` 中复用现有 `Queued`、MSHR 和 source stats，只实现固定大小 context/action 表；每周期 O(1)，epoch 更新只遍历固定 action 数。必须统计 issued/useful/late/unused、带宽和队列 drop，先与 BOP/CMC 做同带宽 A/B。

### PDede：存储高效 BTB

论文：[DOI 10.1145/3466752.3480046](https://doi.org/10.1145/3466752.3480046)。按 region 分区，把相同 target 去重，并以 page 内 delta 编码目标地址，在同一 bit budget 下跟踪更多分支。论文覆盖 100+ 前端受限应用，平均 IPC +14.4%、BTB miss -54.7%。成本是 tag/region 元数据、dedup 查找和 delta 解码关键路径；GEM5 先用固定 `region_bits/target_bits/ways` 的行为模型，保留 BTB lookup latency 和 bank 冲突，不把 storage 增长直接当收益。

## 跨会议实现参考（非 MICRO 2021 候选）

### ATP + SBFP：ISCA 2021 的自适应 TLB 预取

论文：[ATP DOI](https://doi.org/10.1109/ISCA52012.2021.00016)。ATP 组合 stride/PC/distance 预取器并按 miss 动态选择，SBFP 从已取回 cache line 的相邻 PTE 中只预取高收益项。论文报告 Qualcomm/SPEC/GAP 几何加速 16.2%/11.1%/11.8%，并减少 page-walk memory references。GEM5 落点是 `src/arch/riscv/tlb.*`、`src/arch/riscv/pagetable_walker.*` 和有限 PTW queue；开销为 predictor table、PTE filter 和额外 memory requests，验证必须同时看 TLB MPKI、PTW traffic、MSHR 占用和错误预取。

### Voyager：ASPLOS 2021 的分层神经预取

论文：[DOI 10.1145/3445814.3446752](https://doi.org/10.1145/3445814.3446752)。分开学习 page 与 offset 关联，处理不规则访问；论文在无预取器基线上的 SPEC/GAP irregular IPC +41.6%，并称相较先前神经模型计算降低 15-20x、存储降低 110-200x，但仍存在训练/推理时延。GEM5 原型可用小型表/定长乘加延迟表示，禁止每次访问执行无界神经网络；先做 offline-table replay，再逐步加入硬件训练。

## 明确排除或降级

|论文|原因|
|---|---|
|Enabling Branch-Mispredict Level Parallelism|论文明确需要 software hints；摘要虽报平均 +29%，不满足只改硬件。|
|Twig、Ripple|profile-guided BTB/I-cache，需要离线 profile。|
|Trident、Morrigan 的多 GPU/OS 变体|目标是 GPU 或虚拟内存软件栈，不能直接映射当前单核。|
|各类 accelerator/PIM/安全论文|不是通用 KMHv3 CPU 的硬件性能路径。|

## GEM5 实施合同与验证

当前入口：`configs/example/kmhv3.py`、`src/cpu/pred/`、`src/mem/cache/prefetch/`、`src/arch/riscv/tlb.*`。每项先保持旧默认行为，新增 bounded entries、端口/延迟和 backpressure 参数；固定 checkpoint 的 warmup/ROI 后比较 reset stats。最低 stats 为 IPC/simTicks、预测命中/覆盖、队列和带宽占用、前端 bubble 或 PTW latency；不得只引用论文百分比。
