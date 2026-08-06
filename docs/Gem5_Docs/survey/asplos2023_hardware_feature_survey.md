# ASPLOS 2023 面向 KMHv3/GEM5 的硬件特性筛选

> 范围：ASPLOS 2023 Volume 1/2/3（[DBLP V1](https://dblp.org/db/conf/asplos/asplos2023-1.html)、[V2](https://dblp.org/db/conf/asplos/asplos2023-2.html)、[V3](https://dblp.org/db/conf/asplos/asplos2023-3.html)；ACM proceedings [V1](https://dl.acm.org/doi/proceedings/10.1145/3567955)、[V2](https://dl.acm.org/doi/proceedings/10.1145/3575693)、[V3](https://dl.acm.org/doi/proceedings/10.1145/3582016)）。

## 结论

本届目录大部分是系统、GPU、I/O、accelerator 或 memory-management 协同工作。没有一篇能在“不改 OS/二进制/ISA”的严格条件下直接成为当前 KMHv3 P0 feature。最接近的是 Mosaic Pages，但其物理映射约束属于 OS/allocator 合同，故列为条件研究，而非推荐实现。

|状态|论文|论文结论|与当前目标的边界|
|---|---|---|---|
|条件 P2|Mosaic Pages|压缩离散 translation 到一条 TLB entry，TLB miss -6%-81%；28 nm 关键路径可达 4 GHz，但摘要未给绝对面积/bit 数|需要哈希约束的 physical-memory mapping，不是硬件单独启用|
|排除|Occamy|多 CPU 共享 SIMD coprocessor|新增专用协处理器/执行模型，不是现有 O3 feature|
|排除|AfterImage|分析 hardware prefetcher 的安全泄漏|攻击/观测工作，不提供性能机制|
|排除|CXL/memory tiering、I/O cache、NDP/PIM|依赖平台或系统软件|不对单核 CPU A/B 直接成立|

### Mosaic Pages：条件性 TLB reach 方案

[DOI 10.1145/3582016.3582021](https://doi.org/10.1145/3582016.3582021)。Mosaic 利用 virtual contiguity、用 hashing 限制 physical mapping，将多个离散 translation 压缩到一个 TLB entry；论文报告 TLB misses 降 6%-81%，且 28 nm timing/area 分析显示哈希关键路径可达 4 GHz。摘要没有给出绝对面积、TLB entry 位宽或 mapping metadata 的 bit 数，因此不能把“4 GHz”当作低开销证明。论文也明确要求 mapping constraint；这意味着物理页分配/迁移必须配合。若仅在 GEM5 TLB 放大 coverage 而不建模 mapping constraint，就把软件条件错误隐藏了。

可做的前置研究是只读 trace 分析：测当前 RISC-V checkpoint 中 virtual-contiguous but physical-discontinuous 的比例，并给 TLB compression candidate/false-hit/mapping-conflict stats；在没有 OS 修改授权前不实现为默认硬件 feature。

### Occamy 与其他排除项

[Occamy DOI](https://doi.org/10.1145/3582016.3582046) 弹性共享 SIMD coprocessor，适合重新定义多核的 vector 资源而不是改现有 O3 backend。`Re-architecting I/O Caches`、`Pond`、`TPP`、`DeepUM` 都以存储/CXL/运行时环境为前提。`AfterImage` [DOI](https://doi.org/10.1145/3575693.3575719) 是 prefetcher 安全分析；可用来补安全 counter，但没有硬件性能 feature 可移植。

## 行动建议

保持该届为“无严格 P0 候选”的诚实结论。若目标转为跨栈性能研究，Mosaic 可作为 OS+TLB 项目立项；若目标仍是 hardware-only，则优先执行本调研中 PDede、EMISSARY、RFP、ATP/SBFP、PMP 等具备完整硬件因果链的项目。
