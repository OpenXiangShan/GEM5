---
name: branch-predictability-triage
description: 将热点 branch PC 映射到 ELF/源码，结合分支模式判断可预测性与 predictor 改进空间。
---

# 分支可预测性归因

回答 branch PC 属于什么代码、依赖什么输入，以及现有证据更支持 workload 难度还是 predictor 改进空间。

## 核心证据

- 从用户给出的 PC、checkpoint/profile、ELF 和误预测统计开始；核对 ELF 与实际 workload/compiler 是否匹配。
- 用 ELF LOAD 段和运行时地址映射确认归属。高地址只作 runtime/kernel 线索，不能仅凭地址前缀断定归属；不强行用 benchmark ELF 解 runtime PC。
- 定位函数和附近汇编，再关联源码块。缺少行号时仍可做函数级分析，但把候选源码与精确映射区分开。
- 看分支的循环、数据和历史依赖，结合可用动态统计或 trace 判断。源码结构、50% taken ratio 或一次高 mispredict 都不能独立证明“天然难预测”或 predictor bug。
- 若证据不足，给出条件性判断和最小区分实验；不强制把每条分支判为 easy/hard。

## 按需资料

- 查 checkpoint 对应 ELF、工具命令或本地源码路径时：读取 [elf-and-source.md](references/elf-and-source.md)。用户路径优先于环境变量和本机 fallback。
- 需要分支模式启发式或典型案例时：读取 [predictability-examples.md](references/predictability-examples.md)。案例不是普遍规律。

## 输出

按 PC 给出代码归属、ELF/函数、源码或汇编证据、控制流模式、可预测性假设和置信边界。有动态证据时说明它支持或反驳了哪个解释。用户只要求地址定位时，完成映射即可。
