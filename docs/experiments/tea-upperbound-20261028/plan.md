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
- 完整CI验收要求完整0.3c点集、0 abort、正常退出和正式ROI。用户允许先忽略少数慢切片提前估分：冻结各组共同完成点集，保留原cluster权重，标明遗漏点、覆盖率和临时分数；完整结果到齐后补算。新CI创建/排队不等于通过。
- 报告保存每轮配置、假设、分数、benchmark收益/回退、计数器与下一probe；不把事件占比或端口拒绝当成净收益。
- 以[Markdown总入口](README.md)共享报告；定量图插入独立PNG/SVG，流程用Mermaid，保留相对路径以便随仓库阅读。

当前状态：R0两组145点完整，INT26.72579→26.87533（+0.5595%）；保留共同144点的提前估分快照。历史同一1.0c统计换0.3c JSON逐值复现26.72579；采样差异作为已知背景，不计入新优化收益。R1代码已提交并push为515eb0cd1c，构建、CoreMark六模式、四点16组1M检查和Sv39/Sv48边界微测通过；前端20组单变量局部消融已通过。R1三组完整0.3c对照已创建，见runs.json，尚未确认新增配置的完整SPEC分数。

继续入口：先读取runs.json，对每个run核实终态/共享归档完整性，再用标准gem5_data_proc按归档cluster重算分数。已有本地快照与日志在/tmp/tea-upperbound-20261028；不因观察超时重复dispatch。若R1组合未达到26–28，按benchmark/counter残余证据继续资源和时序消融。


## 最新范围调整：保留12宽，独立评估4 load

用户确认astar采样差异为已知背景，后续继续0.3c。用户进一步确认已有12宽，不再推进10整数口的设计方向；原宽后端整包保留作上限参照，不能代表4load单项收益。

旧报告的12路为decode/rename/commit/物理寄存器释放各12；标准scheduler有9个整数IssuePort、3个load口，2 STA/2 STD。当前新增`--upperbound-four-load`只在原scheduler末尾追加ld3及对应wake网络，保留12宽、原FU、原资源和既有IQ索引；会额外增加一个16项load IQ。CoreMark正确性和四pipe激活通过，4个SPEC代表点×原前端/理想前端的8组1M检查全部通过于`/tmp/tea-upperbound-20261028/fourload-spec`。

下一步：两Python文件已提交并push为36584adaf67da1e848d7b3a61d5ab18e84b2c12e；8组完整INI仅出现新ld3及wakeup成员变化，style/precommit通过。4load与4load+frontend完整0.3c已触发，分别为40646873569、40646888439，见runs.json。等待结果后再决定窗口/LSU资源对照，避免重复dispatch。允许资源扩容的原始需求仍保留，窗口/LSU扩容应以独立参数对照评估，不将16宽整包纳入新的4load默认。

原宽后端run1669现145/145成功，INT27.943188743711012，相对基线26.72578513064518提高4.5552%。前端run1671与组合run1670仍运行。20M预热+2M ROI的gcc/mcf两侧TEA已采集并通过四状态及CSV事件守恒；复用脚本在scripts/summarize_tea.py，完整中间输出位于`/tmp/tea-upperbound-20261028/tea-report`，需整理Markdown残余报告。TEA没有PC×四状态字段，不把PC总权重当可移除stall。
