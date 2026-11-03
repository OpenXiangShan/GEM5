# BPU 预测数据模型重构记录

## 目标与约束

从 `75569f588b` 开始，将分支描述、BTB 存储和阶段预测分开，删除重复字段及转换。
保留 `numDelay`、组件优先级、气泡消耗顺序、训练时点、历史更新和 FTQ 行为。
当前任务不重做真实逐级流水，也不改变 PairTAGE 的算法或 two-taken 策略。

## 分阶段执行

1. TAGE 显式使用预测时的基础方向，删除对 BTB counter 的读取及训练时 bool/counter 转换。
2. 分离 BTB 存储表项和预测候选；counter/tag 留在存储侧，方向预测输出明确有效性。
3. 阶段结果使用统一候选数组，消除独立方向/间接目标/RAS 目标映射；组件接口按职责收窄，顶层负责各级组合。
4. 统一公共分支属性与描述，迁移真实分支结果及 PairTAGE，删除重复表示与适配。

各阶段可保留短期适配，最终删除已迁移的旧表示。预测时基础方向与原始目标等训练信息应在 metadata 中显式保存。

## 验证

- 首先从 base 构建独立 baseline binary，保存测试配置、命令、退出原因和完整 stats。
- CoreMark 固定指令窗口；frontendtest 选择条件分支、call/return、动态间接跳转。
- 每阶段构建并运行同一测试集，比较 committed instructions、numCycles、IPC、分支/覆盖/历史相关统计；host 时间和吞吐排除。
- 相关预测器单元测试覆盖 fallback、方向、目标、历史、训练和恢复。
- 最终独立正确性审查；运行 style 检查。
- 最终对 base/head 用相同配置和 SPEC06 0.3c profile 触发 CI，核对实际被测 SHA、coverage、切片和分数。

## 证据记录

本地过程输出存放 `/tmp/bpu-data-refactor/`。阶段结果与 CI run 在完成后补充。
