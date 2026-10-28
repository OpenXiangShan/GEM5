# perlbench_checkspam_5886：正式 CI 20M 供给与残余限制

同一 checkpoint，各组只使用最后完整 20M ROI；分支类错误按 `branchClassMisses` 除 ROI committed instructions 得 MPKI。A/B/C 都保持原 12 宽后端：A 为理想取指、预测宽 1、双取关闭、保留 override；B 为理想取指、预测宽 2、双取开启、保留 override；C 在 B 上关闭 override。4ldFE 是 C 加第四 load，W4ldFE 再加窗口。源文件与精确 PC 的历史匹配仍未证明，0x92e18/0x92e1a/0x96d10 为候选，并非新 TEA 已证热点。

| 模式 | IPC | conditional / indirect jump / indirect call MPKI | decode 空槽 | override 事件 | 第二预测入队 | 双取成功 |
|---|---:|---:|---:|---:|---:|---:|
| baseline | 4.8751 | 1.089 / 0.041 / 0 | 14,239,669 | 426,142 | 不可用 | 0 |
| A | 4.9212 | 1.089 / 0.041 / 0 | 13,888,795 | 426,841 | 0 | 0 |
| B | 5.9762 | 1.050 / 0.037 / 0.00015 | 933,915 | 485,975 | 1,763,605 | 1,765,888 |
| C | 5.9947 | 1.068 / 0.039 / 0 | 730,372 | 0 | 1,776,365 | 1,799,215 |
| 4ldFE | 6.1499 | 1.074 / 0.039 / 0 | 802,291 | 0 | 1,785,326 | 1,787,439 |
| W4ldFE | 6.2489 | 1.073 / 0.038 / 0 | 2,095,623 | 0 | 1,791,554 | 1,789,093 |

A→B IPC +21.44%，B→C +0.31%，baseline→W4ldFE +28.18%。A→B 联合启用每周期第二预测和双取，且保留 override；错误 MPKI 仅约 1.1304→1.0870，IPC 提升不以大幅降低错误次数为必要条件。第二预测、双取计数显著激活且 decode 无后端阻塞时的空槽 13.89M→0.93M，支持此点的前端供给是重要因素；由于 A→B 同时改两开关，现有数据不能给每个开关分配净周期，更不能从计数简单相加。B→C 错误 MPKI 反而略升，收益仍为正。各 PC CSV 可能涵盖 warmup，不能用它代替该末块 ROI 统计。

现有 `smtftqempty`、`ftqNotValid`、`iew.decodeStallReason::FTQBubble` 在这些组均为 0，不能用它们证明没有供给等待；应观察实际 decode 空槽与 predictor/FTQ 供给事件。`decodeBubbles` 是无后端 stall 时未用 decode slots，不是 CPU 周期；`idealSecondPredictionEnqueues` 计成功入队，`twoFetchSuccesses` 计同周期跨预测 taken FTQ 边界，均不能当作净可恢复周期。C 的 `commitOverrideBubbleNum` 仍 356,034，即使 `overrideBubbleNum=0`；不能把前者读作已生效的预测 bubble。

baseline→W4ldFE `commit.ROBFull` 为 69,854→0，IQ full 事件 752,979→946,014，rename fullRegisters 0→0，LSQ full 0→0，decode 空槽反而 14.24M→2.10M；限制随配置变化，不支持单凭某一个计数建立瓶颈排名。B 和 C 的 IQ full 分别 1.032M/1.041M，故继续放宽前端后可能暴露其他限制。D-cache `ReadReq` miss/千指令 baseline 0.556、A 0.564、B 0.573、C 0.564、4ldFE 0.569、W4ldFE 0.584；总 read miss ticks / miss 分别 21,259、21,284、21,541、21,413、19,979、20,919 ticks，属于 cache 事件平均值，不是每条指令净等待，不能解释供给试验差异为 cache miss 数显著下降。

可证伪的后续方向：

1. 在相同 checkpoint、原 12 宽后端和 override 条件下做预测宽度 1/2 × 双取开/关的合法组合，比较 ROI IPC、decode 空槽、入队和双取事件；若只更改某一轴无收益，可排除它单独解释 A→B 的大部分提升。
2. 获取同一 20M ROI 的有边界 TEA / 退休 PC 对齐记录，先验是否集中在 `regexec` 候选以及其 FTQ/decode 供给不足、IQ/ROB/内存等待是否与关键退役窗口重合；若候选 PC 不在新 ROI 热点或重叠证据不足，应放弃以旧 warmup-inclusive PC 排名归因。结果不能从 aggregate 计数直接推断。
