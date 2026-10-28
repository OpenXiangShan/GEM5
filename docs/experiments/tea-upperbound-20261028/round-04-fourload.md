# 第四轮 保留原12宽，独立增加第4个load口

保留24分基线已有的12宽和整数FU拓扑，仅增加第4个load IQ。完整145点INT26.72579→27.07359，提升1.3014%；理想前端背景下，28.99774→29.62660，条件收益2.1687%。原16宽整包只作参照。

## 从旧报告确认范围

旧报告的12路指decode、rename、commit与物理寄存器释放各12/cycle。达标版本`e34d8f6e7901`标准scheduler为9个整数IssuePort对象、3个load ReadPort、2 STA/2 STD。整数口对象支持不同FU，不能按对象数推断任意指令的实际同周期吞吐。

参考源为`/nfs/home/yanyue/tmp/tea-analysis-39234318080/reports/final/gem5-original-final-ideal-resource-comparison-20261026.md`第33、68行，以及`experiments/gem5-architecture-iteration3.md`第298–305、327行；作者原先12宽与16宽受控实验保持了其他资源。未在定向检索中发现已验证的3→4load端口实验。

## 新探针与验证

新增`--upperbound-four-load`：在标准scheduler末尾增加`ld3`，2入端口、16项IQ、1个ReadPort，与ld2相同的延迟和wakeup成员关系；保留既有IQ索引、原整数/FP/vector FU、12宽、ROB352及其他PRF/LSU/缓存参数。因此它增加了一个load口和16项load IQ容量，不能称为仅带宽、零额外容量的实验。

新增IQ显式挂在`scheduler.fourthLoadIQ`下，再放入IQ向量。该做法保证SimObject所有权有效，且原FP IQ[13..16]的CLI参数索引不变。单独探针拒绝与SMT、SpMT、Hybrid和旧宽后端整包同时启用。

CoreMark及四个SPEC代表点×原前端/理想前端共8组均通过difftest和正常maxinsts退出。完整INI逐项对照，只出现新增IQ、IQ列表和三处wakeup成员变化；每组4条load pipe累计接受数均非零。源码独立核对确认dispatch、pipe ID派生、推测wakeup和写回遍历均包含新IQ；累计活动不证明同周期达到四路峰值。

## 短窗口结果与下一步

| 输入 | 原12宽 IPC | 仅4load IPC | 变化 | 理想前端 IPC | 理想前端+4load IPC | 变化 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| gcc | 2.113776 | 2.108090 | -0.269% | 2.246737 | 2.250549 | +0.170% |
| mcf | 0.765291 | 0.783939 | +2.437% | 0.753957 | 0.761082 | +0.945% |
| astar | 3.432317 | 3.372999 | -1.728% | 3.491296 | 3.454469 | -1.055% |
| omnetpp | 0.684971 | 0.682913 | -0.300% | 0.702907 | 0.709716 | +0.969% |

以上均约1M、无预热窗口，用于检查方向；局部收益较小且不一致。不能用原宽后端整包的+4.56%替代第4个load口的单项收益。若完整点集证实收益依赖资源窗口，下一步保持12宽，独立扩大ROB/PRF/LSU，而不是恢复10整数口/16宽作为默认。

代码已提交到`36584adaf67da1e848d7b3a61d5ab18e84b2c12e`，两组完整0.3c均已完成：

- [原12宽+4load](https://github.com/OpenXiangShan/GEM5/actions/runs/40646873569)。
- [原12宽+4load+理想前端](https://github.com/OpenXiangShan/GEM5/actions/runs/40646888439)。

原始局部验证见[data/fourload-local-probes.json](data/fourload-local-probes.json)，完整命令、INI、日志和stats在`/tmp/tea-upperbound-20261028/fourload-spec/`。新增Python配置无需改动C++；沿用已构建的探针二进制完成本地检查，正式CI使用上述精确源码SHA。

完整分数见[data/r4-fourload-full-score.csv](data/r4-fourload-full-score.csv)，逐点验证见[data/r4-r5-complete-audit.json](data/r4-r5-complete-audit.json)。

理想前端＋4load的完整分数见[data/r4-front-full-score.csv](data/r4-front-full-score.csv)，最后两点与全量审计见[data/r4-front-complete-audit.json](data/r4-front-complete-audit.json)、[data/r4-front-last2-audit.json](data/r4-front-last2-audit.json)。两组均为145点；早期143共同点估分已被完整结果替代。
