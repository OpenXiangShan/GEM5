# 设计文档索引

本目录记录 XS-GEM5 中重要机制的设计动机、约束、取舍与跨模块边界。源码仍是实现行为的最终依据。

当前文档入口：

- [XiangShan RTL 与 gem5 的 DTLB 实现对比](mismatch/dtlb-rtl-mismatch.md)：说明 RTL、修改前和修改后的 gem5 如何组织 data TLB，以及 load/store 分离的优势与代价。
- [Kunminghu Frontend 设计文档](frontend/README.md)：Kunminghu v3 前端与 BPU 的顶层演进和各预测器设计。
- [SMT 设计说明](smt/README.md)：线程隔离、共享资源、流水线流控、访存可见性和 FS-SMT 验证路径。

阅读设计文档时，建议先建立模块边界和不变量，再沿文末的代码入口核对当前实现。源码变化后，设计意图与不变量通常比具体行号更稳定。
