# R5 与 R7 审计及标准计分

R5-front、R7-A 均全145点逐点审计通过：maxinst exit0、difftest、ROI约20M、12宽及原整数FU拓扑。R7-B 冻结时143点，全部已审点通过。

R4-front 29.626604；R5-front 30.208462，相对 +1.964%；R7-A 26.973644。

| INT benchmark | R5 vs R4-front |
|---|---:|
| perlbench | +1.475% |
| bzip2 | +0.206% |
| gcc | +0.398% |
| mcf | +3.597% |
| gobmk | +0.962% |
| hmmer | +3.729% |
| sjeng | +2.444% |
| libquantum | -0.031% |
| h264ref | +2.702% |
| omnetpp | +2.711% |
| astar | +5.135% |
| xalancbmk | +0.389% |

R7-B 共同143点配对结果见 r7-common143-comparison.md；不可与完整点集直接比较。逐点证据见 r5-r7-audit.json。
