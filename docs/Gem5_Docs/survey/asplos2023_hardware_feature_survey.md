# ASPLOS 2023：面向 KMHv3/GEM5 的硬件 Feature 全量筛选

> ASPLOS 2023 分为 Volume 1/2/3：[DBLP V1](https://dblp.org/db/conf/asplos/asplos2023-1.html)、[V2](https://dblp.org/db/conf/asplos/asplos2023-2.html)、[V3](https://dblp.org/db/conf/asplos/asplos2023-3.html)；对应 ACM proceedings DOI 为 [V1](https://dl.acm.org/doi/proceedings/10.1145/3567955)、[V2](https://dl.acm.org/doi/proceedings/10.1145/3575693)、[V3](https://dl.acm.org/doi/proceedings/10.1145/3582016)。三卷逐篇扫描，避免漏掉卷间论文。

## 结论

严格要求软件、OS、ISA 和平台不变后，本届没有可直接成为 KMHv3 P0 的完整方案。**Mosaic Pages** 是唯一值得优先做可行性观测的条件候选；其余 CPU 相关论文要么是 accelerator/platform，要么研究安全/分析而非性能。该结论是筛选结果，不是“本届没有硬件论文”。

|状态|论文/feature|论文效果与成本证据|边界与 GEM5 判断|
|---|---|---|---|
|条件 P1|Mosaic Pages|压缩离散 translation 到一条 TLB entry；TLB miss -6%--81%；28 nm 关键路径可达 4 GHz，绝对 area/bit 未见摘要披露|需要 physical-memory mapping/hash 约束；先做 trace 观测，不能只放大 TLB coverage|
|排除|Occamy|多 CPU 共享 SIMD coprocessor|新增协处理器、共享执行模型和 workload|
|排除|AfterImage|分析 hardware prefetcher 的安全泄漏|不是性能 feature，最多指导 side-channel counter|
|排除|Pond、DeepUM、TPP、I/O/CXL/memory-tiering|系统内存/设备平台收益|需要 OS、设备或新 memory protocol|

## 全集扫描和证据纪律

先合并三卷 DBLP 目录，按 `CPU core / branch / cache / TLB / prefetch / memory controller`、`OS/runtime/compiler`、`GPU/accelerator/PIM`、`security/analysis` 分类。主候选必须满足：既有二进制不改；状态、端口、延迟、带宽有界；当前 GEM5 有明确插入点；可用 feature-off/on A/B 和 causal counters 验收。

证据等级：`A`=DBLP/DOI 书目，`B`=摘要或公开作者版明示的机制/数字，`C`=当前源码映射。摘要没给出的绝对存储和面积不补猜；论文数字不外推为 KMHv3 IPC。

## 条件候选详情：Mosaic Pages

论文：[Mosaic Pages DOI 10.1145/3582016.3582021](https://doi.org/10.1145/3582016.3582021)。Mosaic 利用 virtual contiguity 和受限 hashing，把多个离散 page translation 压缩到一条 TLB entry；摘要级结果为 TLB miss 降 6%--81%，28 nm timing/area 分析中关键路径可达 4 GHz。摘要未提供绝对面积、entry bit 数和 mapping metadata，必须保持未知。

**为什么是条件项。** physical page 的分配/迁移必须遵守 mapping constraint；如果 GEM5 只把多个 page 合成一条 TLB entry，却不模拟 allocator 失败、迁移、冲突和额外 hash latency，就会把 OS 合同错误隐藏成“免费硬件”。

**当前 GEM5 落点。** `src/arch/riscv/RiscvTLB.py`、`src/arch/riscv/tlb.cc` 和 page-table walker 是最近似路径。Phase 0 只读取 trace，统计 virtual-contiguous/physical-discontinuous 比例、可合并候选、hash conflict、TLB reach 和 walk cycles。Phase 1 才加入有限 compressed-entry table：每 entry 保存 base VPN、range/bitmap、hash seed、valid/permission；entry merge、split、sfence.vma、fault、two-stage translation 和 checkpoint restore 必须原子。

**验收。** 对比 ordinary TLB、Mosaic metadata-only、完整 mapping-constrained 三组；统计 TLB hit/miss、walk depth、compressed entry hit、false merge/rollback、page allocation failure、PWC/L2 pollution、metadata lookup latency 与 IPC。若没有 allocator 语义，结果只能称为上界，不得引用论文 -81%。

## 三卷边界项与排除项

|论文/方向|保留原因|不纳入当前 CPU feature 的原因|
|---|---|---|
|Occamy|共享 SIMD 资源有通用 CPU 讨论价值|需要新 coprocessor、共享队列和编程接口|
|AfterImage|揭示 prefetcher side channel|攻击/分析，不提供 throughput mechanism|
|Pond、DeepUM、TPP|内存池、unified memory、页迁移可能影响性能|依赖 OS、CXL/异构内存或 runtime placement|
|Re-architecting I/O caches|I/O cache 组织|设备/IO workload 改变，不能在当前单核 A/B 复现|
|NDP/PIM、GPU、NPU、量子、SSD、网络|硬件贡献可能很大|新增 accelerator/protocol，超出 KMHv3 O3 范围|
|编译器、验证、隔离和安全工作|方法学价值|软件或安全目标不是本文硬件性能口径|

## 当前 GEM5 研究闸门

- 入口：`configs/example/kmhv3.py`、`src/arch/riscv/tlb.*`、`src/arch/riscv/pagetable_walker.*`、`src/mem/cache/`。
- compressed TLB entry 的容量、hash/merge/split 延迟、page-walk 端口和 rollback 都必须参数化；默认关闭。
- 固定 checkpoint、warmup/ROI、cache/DRAM 和 core count，reset 后比较 final stats；至少报告 TLB/PWC MPKI、PTW cycles、metadata traffic、allocation conflict 和 IPC。
- 三卷目录、题名/DOI、摘要级数字与当前源码事实分开记录；全文不可读的条目不得补写精确成本。

### Mosaic 的阶段化验证表

|阶段|实现内容|观测量|通过条件|
|---|---|---|---|
|M0 trace-only|不改变 TLB policy，识别可合并 VPN/physical mapping|virtual/physical contiguity、hash conflict、candidate reach、walk cycles|候选比例和收益窗口可重复|
|M1 metadata-only|建立 compressed-entry table 但仍走普通 translation|entry fill/merge/split、metadata lookup latency、false candidate|无功能差异、容量/端口预算固定|
|M2 constrained mapping|模拟 allocator 只提供满足 hash 的 page|allocation failure、migration、rollback、sfence.vma、two-stage walk|映射约束和失败成本可见|
|M3 policy-on|允许 compressed entry 替换 ordinary entry|TLB MPKI、PWC/L2 pollution、PTW latency、IPC|收益不能来自无限 reach 或零延迟 hash|

### 资源、时序和生命周期清单

Compressed entry 至少需要 base VPN、range/bitmap、permission/ASID/VMID、hash metadata、valid/dirty 状态；merge/split 需要固定比较器和 update port。hash 关键路径按固定 cycles 建模，不使用论文“4 GHz”替代当前 clock。`sfence.vma`、page fault、context switch、checkpoint serialize/unserialize、page migration 需要使所有 compressed entries 失效或重编码。若模拟 two-stage translation，guest/host entry 的所有权和 fault rollback 分开计数。

### 三卷候选台账

|卷/主题|代表条目|筛选决定|
|---|---|---|
|V1 CPU/accelerator|Occamy、编程模型/共享 SIMD|新 coprocessor 与接口，排除当前 O3|
|V2 security/analysis|AfterImage|分析 prefetcher leakage，不是性能 feature|
|V3 VM/memory|Mosaic Pages、Pond、DeepUM、TPP|Mosaic 作为 OS+hardware 条件项，其余依赖 OS/异构内存|
|跨平台|I/O cache、CXL、NDP/PIM、GPU/NPU/SSD|协议/设备/workload 全变，排除|

### 结果记录规范

每个三卷条目记录 `conference-volume / title / DOI-or-DBLP / software dependency / evidence grade / GEM5 path / decision`。Mosaic 实验输出 TLB/PWC MPKI、walk depth/latency、compressed-entry hit/evict、hash/allocation failure、metadata traffic、L2 pollution 和 IPC；没有这些 counters 时不接受单一 IPC 结论。论文 -6%--81% TLB miss 与 4 GHz 仅放在论文事实列。

### Mosaic 的位级和状态生命周期

|状态|作用|更新时机|成本/风险|
|---|---|---|---|
|base VPN/range|描述可压缩 translation 区间|merge/split、page allocation|tag 比较、跨 range alias|
|bitmap/hash metadata|记录离散页及物理映射|fill、migration、evict|hash latency、冲突、allocator failure|
|permission/ASID/VMID|保持访问权限与上下文隔离|context switch、sfence/fault|stale permission、restore 错误|
|valid/rollback state|保证 partial merge 原子性|walk response、fault、squash|half-filled entry、duplicate walk|

Phase 1/2 的 compressed entry 必须与 ordinary TLB entry 共享或明确分离 lookup port；不能假设一次 hash lookup 免费完成。若 allocator 无法提供约束页，应记录 fallback ordinary mapping 的额外 walk、迁移或容量损失。这样即使最终决定不实现 Mosaic，也能回答“收益来自 reach 还是来自被隐藏的 OS 映射”这一关键问题。

## 来源

- [ASPLOS 2023 V1/V2/V3 DBLP](https://dblp.org/db/conf/asplos/asplos2023-1.html)、[V2](https://dblp.org/db/conf/asplos/asplos2023-2.html)、[V3](https://dblp.org/db/conf/asplos/asplos2023-3.html)。
- Mosaic/Occamy/AfterImage DOI 见正文；其余排除项以三卷目录为书目证据。
