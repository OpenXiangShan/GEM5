# Trace Mode Deviations (O3 内部改动速览)

目的：记录 trace 分支相对 `origin/xs-dev` 在 O3 流水线中的改动，便于后续回滚/比对。范围仅覆盖 O3 CPU 内部代码与直接依赖的接口，不含前端配置脚本。

## 核心新增能力
- **Trace 模式参数（BaseO3CPU）**：新增 `enableTraceMode/traceFile/traceFormat/traceAddr*`、trace 分支预测开关（decoupled BP）、错误路径注入、mispredict penalty 等参数，并注册 TraceReader 源文件。
- **Fetch 侧 TraceReader 集成**：引入 `TraceReader`、元数据映射（seqNum → TraceInstruction/traceIdx）、wrong-path 注入、trace 统计，支持 Champsim/CBP2025，提供 trace 地址映射/窗口填充/软回滚。
- **CPU 访问 Trace 元数据**：增加 `isTraceInstruction/getTraceInstMetadata/getTraceIndexForSeqNum/getTracePCByIndex/cleanupTraceMetadataOnCommit/getOldestInFlightSeqNum` 便于 commit/调试阶段查询与清理。

## 阶段性改动（按文件）
- **commit.cc/commit.hh**
  - 处理 trace 控制流 fault（TraceCtrlFlowFault），在 commit 时驱动 fetch 回滚并跳过 faulting inst。
  - 为 trace 模式保留 `noSquashFromTC` 辅助逻辑，记录 traceCommitIndex。
  - 在 drain/EOF 时允许 trace 提前清空流水线。

- **cpu.cc/cpu.hh**
  - 追加 trace 元数据查询/清理接口（见“核心新增能力”）。
  - 调整 instList 删除路径以适配 trace 元数据；其他行为保持与上游一致。

- **BaseO3CPU.py**
  - 暴露 trace 参数（格式、BP 训练/验证、错路径注入、地址映射、mispredict penalty 等）。

- **fetch.hh/fetch.cc**
  - 集成 TraceReader、trace 元数据队列/映射、trace 统计。
  - trace 模式下的 BP 训练/验证、wrong-path 注入、FSQ 预充、trace mispredict penalty。
  - 提供对外元数据查询（被 CPU/commit 调用）。

- **dyn_inst.hh/dyn_inst.cc**
  - 为指令携带 traceIndex/traceMetadata 句柄，支持 trace 序列对齐与回滚。

- **issue_queue / inst_queue / iew / lsq / lsq_unit**
  - 兼容 trace 元数据/回滚流程的必要钩子（例如 trace-driven 取消/完成路径）；未改变常规语义，主要是为 trace 触发清理或跳过重复处理。

## 保留与兼容
- 前端配置仍在 `configs/common/xiangshan.py`，运行入口切换为 `configs/example/kmhv3.py`。
- 非 trace 模式下保持与上游行为一致；trace 特性默认关闭，需显式 `--enable-trace-mode`。

## 回滚提示
- 若需完全去除 trace 影响，需移除上述新增参数/接口，并将 fetch/commit/CPU 中的 trace 元数据逻辑一并清理；确保 dyn_inst 的 trace 字段同步删减。

