# Override 气泡模型与理想开关边界

依据当前工作区源码及 `git diff e34d8f6e79 515eb0cd1c` 静态检查；未运行仿真。515eb 在既有 override 生成与等待机制上增加双次串行预测和理想清零开关。

## 已核实的模拟行为

1. `src/cpu/pred/btb/decoupled_bpred.cc:437-467` 每次 lookup 对所有启用组件调用 `putPCHistory`，生成各 stage 预测。`:485-499` 选最后一个有 BTB entries 的阶段，作为最终预测；`:548-561` 从 stage0 开始找最早与最终结果匹配的 stage，`first_hit_stage` 是正常模型要设置的气泡数，并非简单统计 branch mispredict。`:563-594` 保留 S3 更新、override reason/count 及预测源统计。
2. 正常模式 `:599-604` 将 `numOverrideBubbles=first_hit_stage`；`:609-623` 有气泡时延后该预测入 FTQ（代码日志称 FSQ）。每个 tick 在 `:404-410` 至多递减一次，`overrideBubbleNum` 每次递减加1；squash 可在 `:360-363` 清零。所以该计数是该模型实际消耗的 BPU 气泡递减次数，不是 CPU 独占停顿周期。
3. `idealNoOverrideBubbles=true` 把每次 lookup 产生的等待计数设0；选最终 stage、组件预测、历史/训练及 overrideCount 等路径未因开关绕过，预测更快入队但推测时序及后续执行会变化。默认 false（`src/cpu/pred/BranchPredictor.py:1227-1230`）。理想模式不等价于一个已实现的真实 ahead predictor，亦不保证错误率/路径与正常模式逐条相同。
4. `idealOverrideBubblesSaved` 在理想模式每次 lookup 累加该次 `first_hit_stage`（`:599-604`，统计描述 `decoupled_bpred_stats.cc:524-525`）。这是每次预测的**被清零的名义等待量之和**；不是以同一执行路径比较得出的实际少用气泡，更不是净 CPU 周期或可相加性能收益。
5. 双预测模式 `:385-401` 先处理第一 target、推进 PC/history，再满足 `canStartPrediction`（`:284-291`）才进行同 tick 第二次 lookup 并处理；bubble 只在 tick 尾部老化一次。两次 lookup 都可能对 `idealOverrideBubblesSaved` 累加，因此它甚至可在一个 CPU tick 中累加多项；正常气泡限制第一项入队时第二次预测也可能不发生，两侧 lookup 数量、预测顺序和推测历史会变。不可拿该计数直接当节省的 BPU tick 或 CPU cycle。

## 现有 ABTB / ahead 边界

`src/cpu/pred/btb/abtb.hh:30-37` 描述已有 AheadBTB 以前一 block PC 访问、当前 block PC 做 tag 比较，ahead stage 为 1；`abtb.cc:448-470` 有 ahead-read 队列；`decoupled_bpred.cc:650-664,1374-1393` 维护预测历史 PC 并填 ahead 元数据，`:568-572` 用 S3 预测更新 ABTB。它是既有特定预测组件，不等于将所有后续阶段 override 延迟清零；这些代码不足以证明零气泡实现、时序收敛或 RTL 可行性。

## A/B/C 的可比较含义

固定同 checkpoint、20M warmup、完整 0.3c 及其他参数：A=理想指令供给、单预测/单取、正常气泡；B=理想供给、双预测/双取、正常气泡；C=理想供给、双预测/双取、零气泡。B/A 是组合带宽改动在正常气泡下的条件净效果；C/B 是双带宽背景下清除 override 等待的条件净效果，**不是原始配置的单独气泡成本**。请同步报告实际 instructions/cycles、branch errors、`overrideCount`、`overrideBubbleNum`、`idealOverrideBubblesSaved`、second requests/enqueues、two-fetch 和 FTQ供给；对应全配置、coverage及正确性审计。预测与时序可相互影响，不能把 nominal saved 直接转成周期或将各增量相加作因果分解。
