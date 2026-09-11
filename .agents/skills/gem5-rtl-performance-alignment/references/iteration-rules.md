# 迭代规则

本文件只适用于显式 `MODE=autonomous-iterate`；默认模式仍需逐轮确认。

## 启动与预算

用户一次性提供两侧 commit/worktree、已有 GEM5/RTL result、`ARTIFACT_ROOT` 和 `CASE_ID`。不猜 commit；开始前必须记录唯一 profile、共同 checkpoint realpath、两侧 reference SO 路径和后 20M window evidence。

默认预算，可在启动时增大前三项：

```text
MAX_ITERATIONS=8
MAX_CANDIDATE_SLICES_PER_ITERATION=4
MAX_NEW_GEM5_RERUNS_PER_ITERATION=4
MAX_RTL_TRACE_RERUNS=1       # 硬上限
MAX_RTL_TRACE_SLICES=1       # primary slice only
```

## 冻结 RTL

优先复用已有且已证明覆盖后 20M 的 RTL lifetime DB，并记录其 commit、worktree、checkpoint、reference SO hash、schema 和 window evidence。若没有合格 DB，最多运行一次 RTL primary-slice trace；记录任务状态和产物，完成后检查并冻结。之后不修改、不重复运行 RTL，不增加 RTL slice。

## GEM5-only 循环

每轮从用户提供的 GEM5 commit 创建隔离 candidate worktree，只应用一个由 counter + trace + 代码支持的行为改动：

```text
记录 hypothesis 和预测
→ 编译 GEM5
→ 对不超过预算的 slice 跑 40M/20M
→ 保存后 20M stats、counter、lifetime DB 和 inter-gap report
→ 与同一 RTL baseline 比较
→ accepted / rejected / inconclusive
```

不得改变 checkpoint、输入、reference SO、40M/20M、cache/DRAM、difftest 或停止条件来制造收敛；不得自动 commit、push、PR、远程 CI 或清理共享结果。

`accepted` 必须同时满足：关键 gap/热点贡献向 RTL 收敛、相关 counter 按预测变化、总体 IPC 或 weighted gap 收敛且 correctness 正常。只有 IPC 变化而 counter/gap 不变时标记 `inconclusive`。

## 停止条件

立即停止并报告 `reports/iteration_summary.json`：

- profile、checkpoint、SO、commit、window 或 RTL baseline 不可验证；
- 需要第二次 RTL trace、RTL 改动/配置改动或更换 slice/input；
- GEM5 candidate 无法隔离，或 build/run/difftest 失败；
- 需要非单变量改动、额外 instrumentation 或改变比较合同；
- 达到预算，或连续两轮没有收敛且没有更强的新假设；
- 需要任何外部提交、推送、PR、远程 CI 或共享目录操作。
