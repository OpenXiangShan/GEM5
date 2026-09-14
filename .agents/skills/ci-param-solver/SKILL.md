---
name: ci-param-solver
description: 编写或校验 GEM5 SolveSpec 参数搜索，并按请求准备或触发 manual-solve.yml。
---

# CI 参数求解

产出可解析、可绑定的 `configs/solver_specs/<name>.py`；请求触发时，完成预检并回查远端 run。

## 按任务选择入口

- 编写或修改 spec：读取 [spec-authoring.md](references/spec-authoring.md)。
- 查询 DSL、目标函数、工作负载或并行语义：读取 [solver-rules.md](references/solver-rules.md)，必要时查 `util/solver/spec/`、`util/solver/parser/` 和现有 specs。
- 准备或触发 CI：读取 [ci-dispatch.md](references/ci-dispatch.md)，以目标 ref 的 `.github/workflows/manual-solve.yml` 为输入接口依据。
- 验证本 skill 的行为：使用 [test-scenarios.md](references/test-scenarios.md)。

不必为只改 spec 的任务先读取完整用户指南和远端 workflow。文档与源码不一致时，以对应版本源码为准。

## 信息与动作边界

- 用户决定搜索目标、候选域、约束、工作负载和预算；沿用本次任务已明确的选择或明确指定的 spec，不编造实验语义。
- 参数对象路径、类型、合法取值及统计名先从配置、源码、已有 spec 或实际 stats 查明；只有歧义无法消除时才询问用户。
- 缺少影响实验含义的选择时，只暂停依赖它的动作。继续代码调查、现有 spec 解析或已明确部分的草稿；草稿注明未决项，不把未完成 spec 当作可执行搜索。
- 缺少 CI 引用或并发设置不阻塞已明确的本地 spec 工作。CI 默认值以目标 workflow 为准，展示最终生效值；覆盖用户已有选择的冲突需先解决。
- 只生成/校验/预览时完成本地工作并返回结果。用户已明确要求触发时，预检通过且 spec 位于目标远端引用后直接执行，不再次要求“现在触发”的确认。
- 不把任务授权扩展到无关推送、额外搜索或重复 dispatch。

## 必须保留的实验约束

- 精确绑定参数路径和类型；耦合参数通过 `apply_trial()` 映射，避免独立搜索生成非法组合。
- 多目标为 Pareto 语义；`bayes`/`ga` 只支持单目标，算法支持情况以当前实现为准。
- `custom_bin` 不能同时使用 `score_txt` 或 `specific_benchmarks`；当前求解运行时不支持 SMT 工作负载。
- `max_trials` 等停止条件必须有来源；不把临时短跑参数带入正式搜索。
- CI 输入可覆盖 spec，预检和触发必须采用同一组最终生效值。
- 解析成功、绑定通过、run 已创建、搜索完成是不同状态；报告实际验证边界。

## 完成标准与工具

本地任务返回 spec 路径、参数/域、目标、预算及解析/绑定结果。触发任务还返回 run URL、实际引用、状态和结论；只要求触发时确认 run 存在即可，不默认等待完整搜索结束。

- `scripts/solver_ci_dispatch.py`：校验、dry-run、dispatch 和回查。
- `scripts/self_test.py`：离线输入回归，不触发 CI。
