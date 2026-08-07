# HPCA 2022：面向 KMHv3/GEM5 的硬件 Feature 全量筛选

> 书目入口：[HPCA 2022 DBLP 目录](https://dblp.org/db/conf/hpca/hpca2022.html)。目录逐篇初筛；DOI/公开摘要支持论文事实，当前源码只支持 GEM5 映射。论文数值不外推为 KMHv3 结果。

## 结论

HPCA 2022 提供了三个可独立验证的局部方向：**cache-level prediction**（隐藏 load miss latency）、**Mockingjay**（有限 PC reuse-distance replacement）和 **GBDI**（有成本的 compression）。Runahead 与 AVA 也属硬件-only，但需要完整 recovery 或 RVV 项目，故排在后面。

|优先级|feature|论文效果与成本|当前 GEM5 判断|
|---|---|---|---|
|P0|Cache Level Prediction|平均 access latency -20%，性能 +10.3%（常规）/+6.1%（boosted）|LSQ/多级 cache 并行请求；需处理重复响应和错误流量|
|P0|Mockingjay|相对 LRU 单核 +5.7%，无 prefetch 多核 +15.2%，有 prefetch CVP +20.1%|replacement-local prediction，适合 iso-storage A/B|
|P1|GBDI|compression ratio 2.3×、带宽 1.5×、SPEC17 高带宽 workload +10%|必须建 compressor latency、格式和 DRAM bytes|
|P1|Reliability-Aware Runahead|memory-intensive +33.5%，MTTF 4.8×|核心 recovery/precise state 高风险，先做受限 prefetch 原型|
|P2|AVA|8 KB VRF + IQ；短向量约 2×、相对长向量基线面积 -50%|需要 RVV/vector model，不是 scalar KMHv3 首批项目|

## 全集扫描与证据规则

从 HPCA 2022 DBLP 目录完整扫描 CPU、cache、prefetch、translation、DRAM、vector 和 memory reliability 主题；对 GPU/PIM/NoC、OS/runtime、ISA/compiler、设备/系统和安全论文逐一记录边界。主候选需同时满足软件不变、有限状态/带宽/时延、可挂接当前 KMHv3 路径、可通过 counters 验收。

`A`：DOI/DBLP 书目；`B`：摘要/作者公开版给出的机制和数字；`C`：源码映射。摘要未披露的 predictor entry 位宽、端口、compressor 面积或能耗不做推断。

## 主候选详情

### P0：Cache Level Prediction（CLP）

论文：[DOI 10.1109/HPCA53966.2022.00054](https://doi.org/10.1109/HPCA53966.2022.00054)。CLP 预测 load 会在哪一级满足，在普通 L1 lookup 完成前对预测的 L2/LLC/memory 提前发起请求；论文报告平均 access latency -20%，性能 +10.3%，对 boosted baseline 仍 +6.1%。

**最小合同。** 每个 PC predictor 给出 target level/confidence；早发请求占用真实的 MSHR、cache port、interconnect 与 DRAM queue。普通 L1 请求、预测请求和合并响应必须只有一个 owner；错误预测造成额外流量/污染，store、squash、uncacheable/MMIO 与 replay 不能绕过一致性。

**GEM5 落点与验证。** 在 `src/cpu/o3/lsq.*`/cache request path 中添加 sideband，复用现有 `Packet`、MSHR 及 `retryMem()` 反压语义，不另造无限请求通路。比较 `baseline / observer-only / CLP`；报告 correct/wrong target、saved cycles、duplicate merge、extra L2/DRAM request、queue full、load-use stall、MPKI 与 IPC。若当前模型没有可并行的中间层，请先定义时序而不是把 memory response 提前送回。

### P0：Mockingjay

论文：[DOI 10.1109/HPCA53966.2022.00048](https://doi.org/10.1109/HPCA53966.2022.00048)。它使用 PC 分类的多类 reuse-distance 状态排序 insertion/victim，近似 Belady 类优先级；摘要级结果为单核相对 LRU +5.7%，无 prefetch 多核 +15.2%，有 prefetch 的 CVP +20.1%。

**GEM5 合同。** 在 `src/mem/cache/replacement_policies/` 以固定 tag/counter/epoch/way scan 实现，保持同一 cache capacity、associativity 和 lookup latency。预取 fill 与 demand fill 要有独立训练/插入策略，不能把 prefetch 污染藏入训练。统计 predictor hit/collision、distance class、victim reason、demand/prefetch reuse、MPKI、traffic；单核先证实局部收益，多核/LLC 结果不从单核外推。

### P1：GBDI

论文：[DOI 10.1109/HPCA53966.2022.00085](https://doi.org/10.1109/HPCA53966.2022.00085)。GBDI 用跨 block global base 做 BDI，后台 clustering 选择 bases；公开摘要给 compression ratio 2.3×、bandwidth 1.5×、SPEC17 的高带宽 workload 平均 +10%。

需要限制 base-table size、compressed format、compress/decompress port、flush/writeback expansion 与 DRAM transfer bytes。模型应把压缩成功率、CPU stall、metadata access、extra writeback 和有效容量一并报告；不能只把物理 memory 容量扩大后引用 +10%。当前落点是 L2/内存控制器之间，建议先作 trace/replay，在不改变 core 时序的前提下确认 traffic 因果。

### P1：Reliability-Aware Runahead；P2：AVA

Reliability-Aware Runahead [DOI 10.1109/HPCA53966.2022.00062](https://doi.org/10.1109/HPCA53966.2022.00062) 早启动 runahead，隔离等待 memory 时的易错微状态，论文 memory-intensive +33.5%、MTTF 4.8×。先建立只产生 prefetch、不改变 architectural state 的 precise runahead；rename/ROB/LSQ checkpoint、store ordering、fault、squash 任何一项不完整都不能开性能实验。

AVA [DOI 10.1109/HPCA53966.2022.00063](https://doi.org/10.1109/HPCA53966.2022.00063) 是可重配 MVL/VRF/IQ 设计，只有在 RVV execution、vector register file 和 vector issue 全部在 scope 内时才是候选；当前 scalar-only A/B 不应以提高 scalar width 代替它。

## 边界与排除

|方向|处置理由|
|---|---|
|CRISP、tākō 等|需要 software hint、callback 或 ISA/programming-model，不能在既有二进制上单独启用|
|GPU/PIM/NoC/量子/accelerator|新增处理器/平台，当前 O3 不能局部映射|
|OS placement、persistent memory、系统资源管理|利益依赖 kernel/runtime/protocol 改变|
|安全/可靠性-only 机制|没有对应故障/攻击基线时不是性能增强；需另建实验平台|

## 统一实施与验收

- `kmhv3.py` 固定 L1/L2、DRAM、core count、prefetcher 和 checkpoint；warmup 后 reset stats。
- 所有 P0/P1 状态参数化并默认关闭，记录 metadata bit/entry、lookup/update latency、queue/port contention、drop/retry。
- 最低报告 IPC、load latency、L2/DRAM MPKI/traffic、MSHR/queue occupancy 与 feature-specific correct/wrong/useful/late；论文数字仅作背景。
- 来源：[HPCA 2022 DBLP](https://dblp.org/db/conf/hpca/hpca2022.html)，各 DOI 见正文；未取得全文的成本字段保持 unknown。

### P0/P1 实施分阶段

|阶段|实现|关键闸门|
|---|---|---|
|CLP-0|只记录每个 load 的最终满足层级|PC/region predictor 的 target/confidence 可从实际 response 学到|
|CLP-1|预测层级但不提前发请求|预测正确率、lookup latency、MSHR ownership 不改变|
|CLP-2|并行请求开启|duplicate merge、wrong-target traffic、port/queue contention 可见|
|Mockingjay-0|离线统计 reuse-distance class|不把 Belady future 信息泄漏进运行时|
|Mockingjay-1|固定 predictor/replacement|同 cache bits/ways/latency，prefetch fill 单独记账|
|GBDI-0|trace/replay compression|format、base-table、compress/decompress cycles 固定|
|GBDI-1|接入 L2/DRAM traffic|writeback expansion、metadata request、带宽/容量公平|
|RAR/AVA|先做功能 observer|precise recovery 或 RVV/vector model 未完成前不跑 IPC|

### 资源预算和典型失败模式

CLP 错误预测的请求即使最终被 L1 hit 取消，也可能已经占用 MSHR、cache port、interconnect 或 DRAM queue；统计 cancel/drop 不能省略。Mockingjay 的 reuse-distance predictor 需要固定 tag/counter、update epoch 和 victim scan；不能使用无限 trace history。GBDI 的 2.3× compression 不能替代 metadata/格式、compressor pipeline 和 writeback expansion。Runahead 的 checkpoint 必须覆盖 rename/ROB/LSQ/store ordering；AVA 只有在 vector RF/issue/decoder 与论文同层级后才可评估。

### 目录台账与边界

|主题|已保留条目|最终决定|
|---|---|---|
|cache prediction/replacement|Cache Level Prediction、Mockingjay|P0，当前 cache/LSQ 可挂接|
|compression|GBDI|P1，先验证 bandwidth/capacity 因果|
|core speculation|Reliability-Aware Runahead|P1 observer/受限 prefetch，功能风险高|
|vector|AVA|P2，需 RVV 项目 scope|
|software/ISA|CRISP、tākō、compiler/runtime work|软件接口不可省略，排除|
|platform/accelerator|GPU/PIM/NoC/quantum/CXL/SSD|新平台，排除当前 O3|

### 统一结果字段

每个 workload/phase 输出 predictor correct/wrong、saved/extra cycles、L1/L2/DRAM MPKI、MSHR/queue、prefetch interaction、compress ratio/bytes/latency、runahead recovery 或 vector occupancy，再给 IPC/simTicks。表项扩容必须与 metadata bits、ports、timing sweep 同步；不能只报告最优配置。

### 论文基线到 GEM5 基线的映射

|论文基线|当前树的最近似|不能直接等同的部分|
|---|---|---|
|CLP 多级 cache|KMHv3 L1/L2、LSQ、MSHR|论文 target-level/并行端口与当前层级可能不同|
|Mockingjay LRU/LLC|`XSDRRIPRP`/每核 L2|共享 LLC、多核 prefetch interaction、policy latency 不同|
|GBDI compressed memory|L2/DRAM controller|论文 compression format、容量、后台 clustering 未必存在|
|RAR runahead|O3 ROB/LSQ/rename|可靠性 checkpoint 与 precise recovery 需新增|
|AVA vector RF|RISC-V RVV path|scalar KMHv3 没有同等 VRF/IQ/vector timing|

任何 A/B 报告必须同时列论文 baseline 与 current GEM5 baseline；如果先把 current policy 换成 LRU 再实现 Mockingjay，结果只能标为“相对 LRU 的消融”。同理，GBDI 需固定 raw capacity 和 transfer protocol，CLP 需报告错误预测的 extra traffic，RAR/AVA 未完成完整功能时只发布 observer 结果。
