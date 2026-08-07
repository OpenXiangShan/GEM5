# MICRO 2023：面向 KMHv3/GEM5 的硬件 Feature 全量筛选

> 书目入口：[MICRO 2023 proceedings DOI 10.1145/3613424](https://dl.acm.org/doi/proceedings/10.1145/3613424) 与 [DBLP 目录](https://dblp.org/db/conf/micro/micro2023.html)。主表只含 MICRO 2023 论文；EMISSARY 属 ISCA 2023，放在跨会议参考节。

## 结论

本届覆盖 vector-like runahead、prefetch admission/control 和 cache-backed translation。风险调整后的顺序是：先做 **Micro-Armed Bandit**（小状态控制器）或 **CLIP 的 observer/admission**，再做 **Victima**（RISC-V PTW/L2 lifecycle 完整后），最后考虑需要轻量子线程和动态向量化的 **DVR**。论文最大 speedup 并非当前优先级：DVR 的 2.4×包含新执行引擎，Victima/CLIP 的资源及平台假设必须保留。

|优先级|feature|论文效果/成本|GEM5 判断|
|---|---|---|---|
|P0|Micro-Armed Bandit|相对 Bingo/MLOP +2.6%/+2.3%，100 B；SMT fetch +2.2%|最小预取 admission/degree manager，易做 bounded A/B|
|P1|CLIP|critical-load predictor 准确率 >93%；64-core/8-channel 上 Berti 相关有效性 +24%/+9%；1.56 KB/core|先做 criticality observability，单核不外推带宽受限结果|
|P1|Victima|native/virtualized +7.4%/+28.7%，area/power +0.04%/+0.08%|PTW/L2/TLB metadata 协同；RISC-V source tag 是前置条件|
|P2|DVR|1139 B；相对 5-wide OoO 2.4×、Vector Runahead 2×|轻量 in-order子线程仍是新 execution engine，需严格约束|

## 全集扫描与证据边界

从 MICRO 2023 目录逐题名初筛，将 core/cache/prefetch/TLB/memory 论文留在候选池，再检查二进制/OS/ISA 不变、有限状态与端口、当前路径和可验收 A/B。`A`=DOI/DBLP 书目，`B`=摘要/作者公开版，`C`=GEM5 映射。摘要级机器配置/带宽/成本不能被转化成当前单核预测。

## 候选详情

### P0：Micro-Armed Bandit —— 极小在线控制器

论文：[DOI 10.1145/3613424.3623780](https://doi.org/10.1145/3613424.3623780)。该工作利用时间窗口 action space 的 temporal homogeneity，以 bandit 代替大 RL 表管理 prefetch，报告相对 Bingo/MLOP +2.6%/+2.3%，硬件状态 100 B，并展示 SMT instruction fetch +2.2%。

**建模合同。** action 数、epoch、reward、exploration、history、saturating counter、每周期更新/选择次数均固定；reward 只能来自已完成的 useful/late/unused、traffic、queue-full 等事件，不能从未来 demand 反查。manager 只改变已有 prefetcher 的 admission/degree/source selection，不替代其 address generator，也不能绕开 MSHR/backpressure。

**GEM5 A/B。** 把控制器置于 `src/mem/cache/prefetch/` composite worker 或单个 prefetcher wrapper，比较 `off / fixed degree / bandit`；固定 prefetch queue、MSHR、DRAM、table size。统计 action/epoch/reward、issue/useful/late/unused、bandwidth/queue drop、pollution、IPC。SMT fetch 结果需单独多线程前端基线，不能从单核结果推断。

### P1：CLIP —— bandwidth-aware critical load isolation

论文：[DOI 10.1145/3613424.3614245](https://doi.org/10.1145/3613424.3614245)。CLIP 识别已有 prefetcher 仍无法及时服务、且会造成 stall 的 critical loads，避免把带宽用于已被准确覆盖的请求。论文给 criticality prediction >93%、1.56 KB/core；在 64-core/8-channel 情景报告 Berti 相关有效性 +24%/+9%。

**范围与合同。** 当前单核可能没有论文的带宽压力，故先做 observer：PC criticality table、confidence、train window、lookup/update port 都有界，记录 demand/prefetch overlap、critical stall、MSHR/DRAM queue。接着只把它作为 prefetch admission/filter，保持 Berti/其它 generator 不变；比较 off、observer、CLIP. 不可把“critical”负载直接给零等待或 DRAM 优先级而不扣其他请求的排队代价。

### P1：Victima —— L2-backed costly translation

论文：[DOI 10.1145/3613424.3614276](https://doi.org/10.1145/3613424.3614276)。Victima 预测 PTW cost，将昂贵 translation 以 cache-backed entry 形式在 L2 保留，并用 TLB-aware replacement 控制 data-cache 污染；作者公开版可核对 native/virtualized +7.4%/+28.7%、area/power +0.04%/+0.08%。

**当前 GEM5 前置条件。** RISC-V `Request`/walker 必须可靠携带 PTW 及 instruction/data source；L2 fill/evict、TLB/PWC entry、cache block metadata 和 `sfence.vma`/fault/restore 生命周期需要同一所有权。按地址猜 PTE 或只增大 L2TLB 都不是 Victima。

**分阶段验证。** Phase 0 加 PTW source/cost observability；Phase 1 只记录 prospective cached translations，不改变 policy；Phase 2 在固定 L2 capacity 添加 bounded Victima entries；Phase 3 加 TLB-aware replacement。统计 PTW count/latency、TLB/PWC hit、Victima hit/evict、data victim/reuse、L2 MPKI、MSHR/DRAM traffic 和 IPC。任何 data pollution 必须与 page-walk 收益一起呈现。

### P2：DVR —— Decoupled Vector Runahead

论文：[DOI 10.1145/3613424.3614255](https://doi.org/10.1145/3613424.3614255)。DVR 用与主线程解耦的 speculative in-order subthread 推断 loop bound、识别 stride/间接访问并动态调节 vectorization degree；论文报告 1139 B、相对 5-wide OoO 2.4×、相对 Vector Runahead 2×。

第一版只能是**有界 prefetch engine**：context 数、launch/flush latency、lane 数、max iterations、branch divergence、address fault、MSHR/queue budget 和 throttle 明确；不得将所有间接链提前知晓。可在 `src/mem/cache/prefetch/` 先建 DvrPrefetcher trace/replay，再决定是否接入 O3/rename。验收除 accuracy/coverage 外，必须报告 engine active/stall/abort、degree distribution、bandwidth 和 cache pollution。

## 跨会议参考与排除

**EMISSARY** 是 [ISCA 2023 DOI](https://doi.org/10.1145/3579371.3589097) 的 L2 instruction replacement，详见 `isca2023_hardware_feature_survey.md`，不能写成本届候选。

|方向|严格 hardware-only 决策|
|---|---|
|Clockhands|需要 rename-free ISA/软件约束，排除|
|Ignite|serverless invocation CFG metadata/运行时是前提，排除|
|Utopia/Mosaic Pages 类|OS huge-page/physical mapping，跨栈项目|
|GPU/PIM/NPU/accelerator/security|平台/协议/目标改变，非当前 O3 transparent feature|

## 统一验证与来源

- 路径：`configs/example/kmhv3.py`、`src/mem/cache/prefetch/`、RISC-V TLB/PTW、L2 replacement。
- 所有 entries、bits、latency、ports、queues 参数化、默认关闭；warmup 后 reset stats。
- 统一报告 IPC、demand MPKI、prefetch/translation/cost-specific counters、traffic、MSHR/queue/port 和回退原因；论文数字只作原平台背景。
- 来源：[MICRO 2023 DBLP](https://dblp.org/db/conf/micro/micro2023.html) 与正文 DOI；未获得全文的细节一律标 unknown。

### 分阶段实施表

|阶段|对象|范围|停止条件|
|---|---|---|---|
|MAB-0|现有 prefetch observer|记录 action、epoch、reward、bandwidth|确认 temporal homogeneity，而非先假定收益|
|MAB-1|admission/degree manager|100 B 等效 table、固定 arm/update|queue/traffic/accuracy 可解释|
|CLIP-0|criticality observer|PC/history、stall overlap、MSHR/DRAM|当前单核是否有 bandwidth-critical loads|
|CLIP-1|admission filter|不改变原 generator|critical false positive、demand/prefetch overlap|
|Victima-0|PTW source/cost trace|Request sideband、PTE/L2 hit|所有 normal/retry/two-stage 路径一致|
|Victima-1|prospective metadata|不改 policy|entry lifecycle、data pollution|
|Victima-2|TLB-aware replacement|fixed L2 capacity|PTW gain 净化 data victim|
|DVR-0|decoupled engine observer|trigger/loop/stride/degree|不影响 architectural state|
|DVR-1|bounded speculative requests|lane/max iteration/queue|fault/divergence/abort/backpressure|

### 关键成本和错误路径

MAB/CLIP 的小 storage 不代表零时序：counter update、reward sampling、criticality lookup、admission decision 和 queue backpressure 都要建固定 cycles。Victima 的 area/power 百分比依赖论文平台，当前 GEM5 需显式加入 PTW source/cost metadata、L2 ownership、TLB/PWC eviction、`sfence.vma`、fault、squash 和 checkpoint restore。DVR 的 1139 B 也不等于无限 parallel lanes；engine active、lane occupancy、degree、MSHR/DRAM bytes、branch divergence 和 abort 原因需统计。

### 目录审计台账

|主题|代表性条目|处置|
|---|---|---|
|prefetch control|Micro-Armed Bandit、CLIP|P0/P1，当前 prefetch path 可观测|
|translation/cache|Victima|P1，RISC-V source tag 前置|
|decoupled vector|DVR|P2，需要独立 bounded engine|
|ISA/runtime|Clockhands、Ignite|rename-free ISA/CFG metadata/运行时，排除|
|OS/mapping|Utopia、Mosaic Pages|OS/physical mapping 合同，跨栈参考|
|platform/security|GPU/PIM/NPU/accelerator/security|目标系统改变，排除|

### 统一结果字段

所有 profile 使用相同 checkpoint、warmup/ROI、prefetcher、L1/L2、DRAM、core count；报告 MAB action/reward、CLIP critical hit/false positive、Victima PTW/TLB/L2 hit/evict、DVR engine/degree/abort，以及 common MPKI、MSHR/queue/DRAM traffic、port contention 和 IPC/simTicks。论文 +2.6%/+2.3%、>93%、+7.4%/+28.7%、2.4×/2× 只作背景，不设当前 GEM5 通过阈值。

### profile 组合与互斥关系

MAB 应先作为已有 prefetcher 的 manager，不能同时替换 address generator；CLIP 应先固定 Berti/现有 generator，只改变 admission；Victima 应先固定 L2 replacement，再启用 translation metadata；DVR 不能与普通 prefetch degree 叠加而不记总 bandwidth。组合实验顺序为单 feature、observer-only、two-feature interaction，且为每项记录 source tag、queue ownership、MSHR merge 和 throttle reason。

### 回归微测试

MAB/CLIP：action epoch 切换、reward 延迟、bandwidth saturation、queue full、prefetch cancel；Victima：PTW normal/retry/two-stage、PTE L2 hit/miss、TLB eviction、sfence/fault、data pollution；DVR：loop bound、indirect chain、lane divergence、memory fault、squash、engine abort、checkpoint restore。任何 feature-on 只在完整 negative path 通过后才进入长 checkpoint。
