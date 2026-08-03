# Kunminghu Frontend 设计文档索引

文档站点首页：<https://xs-gem5.readthedocs.io/zh-cn/latest/>

本文档目录主要记录 Kunminghu v3 前端 / BPU 中几项关键设计决策，重点回答“为什么这样设计”和“实现上受什么约束”，而不是逐函数解释代码。

建议阅读顺序如下：

1. `bpu_top_level.md`
2. `phr_design.md`
3. `mbtb_design.md`
4. `btb_tage_design.md`
5. `mgsc_design.md`
6. `ubtb_design.md`
7. `abtb_design.md`
8. `microtage_design.md`
9. `uop_cache_design.md`

各文档当前大致分工如下：

- `bpu_top_level.md`：Kunminghu v2 FTB 到 Kunminghu v3 BTB 的顶层演进动机
- `phr_design.md`：顶层 PHR 的语义、更新方式与 folded history 关系
- `mbtb_design.md`：`32B` 存储粒度、half-aligned 查询和 `victimCache`
- `btb_tage_design.md`：多分支 fetch block 下 `BTBTAGE` 的 index/tag/2-way 设计
- `mgsc_design.md`：挂在 `BTBTAGE` 后级的 statistical corrector
- `ubtb_design.md`：最前级 taken-target predictor
- `abtb_design.md`：ahead pipeline 下的较大容量早期 target predictor
- `microtage_design.md`：放在 `S1` 的轻量方向预测器
- `uop_cache_design.md`：O3 uop cache 的动机、实现、bypass 时序与理想化边界

当前暂未单独展开的模块包括：

- `ITTAGE`
- `RAS`

如果后续继续补文档，建议仍沿用这一目录中的写法：

- 先解释动机、约束和取舍
- 再给出少量实现锚点
- 尽量避免把设计文档写成源码逐行翻译
