# 第零轮 固定0.3c点集和24分基线

采用已测量e34d8f6e7901作为共同源码，配置kmhv3.py。此处保留1.0c历史定位，但只以本轮0.3c完整结果比较后续变体。

| 对照 | Actions | 变化 |
| --- | --- | --- |
| Baseline24 | [40638589458](https://github.com/OpenXiangShan/GEM5/actions/runs/40638589458) | 原24.36957配置，额外参数为空 |
| Best24 | [40638592517](https://github.com/OpenXiangShan/GEM5/actions/runs/40638592517) | TAGE16、4个FP IQ各32项、L2 data bank4 |

两侧profile均为spec06-rva23-novec-gcc16-0.3c。归档cluster与checkpoint列表为相同的145点，SHA256为1e8e850e94dba6f7903c0e28c3ff365b54b380221dc4deaf63e7d72f3596f5b7。REF固定到d30fff1ece9e-gem5-r3-multi16g-zfa-cbo/normal-dedup。

最近快照详见[data/r0-progress.json](data/r0-progress.json)，仅表明运行进度，不代表完成或得分。等待完整点集、0 abort、正确退出和score.txt后填写结果；目前不报告新SPEC分数。
