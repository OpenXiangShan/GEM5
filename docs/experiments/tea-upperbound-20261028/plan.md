# TEA upper bound 探索计划

目标是在固定 SPEC06 GCC16 RVA23 无向量 0.3c 点集上，将整数分数探索到 26–28/GHz。允许资源扩大、时序与端口限制放松；所有结果必须保留正确性、完整点集、实际配置与同窗口计数器证据。不能把目标分数写成已实现结果。

## 基线和实现边界

- 用户允许直接使用 24 分分支，因此从已测量的 e34d8f6e7901 建立 `codex/tea-upperbound-24base-20261028`。
- R0 默认：原 24.36957/GHz 的配置，0.3c 重测 CI40638589458。
- R0 高资源：在同一源码上使用 TAGE16、四 FP IQ 各32项、L2 data bank4，0.3c 重测 CI40638592517。历史1.0c的24.50935只作定位，不能替代本轮结果。
- 保留原默认行为；新探针显式启用。全程保留 difftest，使用 pinned normal-dedup REF。

## 探索轮次

1. R0 统一点集基线：核验实际 SHA、配置、ROI、abort/completed、score与coverage。
2. R1 后端：单线程10个整数输出口、4 load、2 STA/2 STD；增加ROB、PRF、LSQ/IQ和前后端供给。明确10指整数输出端口，不是所有操作合计每周期10条。
3. R1 前端：每周期在第一项更新推测状态后串行调用BPU第二次；与PairTAGE互斥。额外独立探针移除override latency。第二FTQ项保留独立metadata/history/RAS恢复。
4. R1 理想双取：沿用BPU结果，最多跨一个taken边界，可为任意目标；仅按实际使用的指令字节做Execute功能翻译与一致的内存读取。单独说明ITLB/Icache理想化，不查询REF未来控制流。
5. R2 组合：base、backend、frontend、backend+frontend对照；按计数器和benchmark回退决定下一步，补资源/时序消融。
6. R3 继续推进残余瓶颈直至26–28目标得到验证或有可审核证据解释搜索结果；每轮写中文图文报告和最终整体报告。

## 验证与报告

- 构建、启动配置、CoreMark及代表checkpoint difftest；相关动态统计证实4个load管线与第二预测/第二fetch实际使用。
- 前端关注第二目标为向后/远跳、FTQ仅一空位、第一/第二项squash、call/return、跨页压缩与32位指令、Execute权限和fault PC/tval。
- baseline关闭新探针时保留原代码路径；需要同源码开关A/B证明统计口径与行为。
- CI仅接受完整0.3c点集、0 abort、正常退出和正式ROI后算分。新CI创建/排队不等于通过。
- 报告保存每轮配置、假设、分数、benchmark收益/回退、计数器与下一probe；不把事件占比或端口拒绝当成净收益。

当前状态：R0两组已创建，尚未获得最终结果；R1正在实现。没有已验证的新分数。
