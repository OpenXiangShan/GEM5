# 设计文档索引

本目录记录 XS-GEM5 中重要机制的设计动机、约束、取舍与跨模块边界。源码仍是实现行为的最终依据。

当前文档分为三组：

- [Kunminghu Frontend 设计文档](frontend/README.md)：Kunminghu v3 前端与 BPU 的顶层演进和各预测器设计。
- [SMT 设计说明](smt/README.md)：线程隔离、共享资源、流水线流控、访存可见性和 FS-SMT 验证路径。
- [TS-BOP 单 BOP Degree 研究与开发跟踪](ts_bop_degree_study.md)：双 BOP 合并、Student degree、离线覆盖率验证和阶段进度。

阅读设计文档时，建议先建立模块边界和不变量，再沿文末的代码入口核对当前实现。源码变化后，设计意图与不变量通常比具体行号更稳定。
