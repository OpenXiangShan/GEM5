# 当前12宽上限配置

完整SPEC06 GCC16 RVA23 novec 0.3c：**INT30.20846/GHz，相对同口径基线26.72579提高13.0311%**。145点均完成，20M预热＋20M正式测量。新增配置的1c约27.5分只是按相对增益粗估，没有实测新配置1c。

性能源码固定为`36584adaf67da1e848d7b3a61d5ab18e84b2c12e`；[完整CI](https://github.com/OpenXiangShan/GEM5/actions/runs/40648104599)，[标准计分](data/r5-front-full-score.csv)，[逐点审计](data/r5-r7-audit.json)。精确workflow输入、REF与归档身份见[runs.json](runs.json)。后续报告提交不改变这次被测源码。

## 配置保持与改变

保留原decode/rename/commit/物理寄存器释放宽度12，原整数/FP/vector FU与原IQ时延。增加第4个load口和独立16项load IQ，保留2 STA/2 STD；容量仅放宽ROB、整数/浮点PRF、LQ、SQ、RARQ、RAWQ七项。缓存MSHR、L2/L3容量及其它未列参数沿用基线。

前端为串行双预测、理想双取、functional Execute指令供给和零override气泡。它是用于探索上限的模型，不能把结果直接称为RTL双前端或ahead pipeline已实现的收益；[完整供给/气泡拆分](round-07-override.md)另行报告。

在`configs/example/kmhv3.py`之后附加以下参数：

```text
--upperbound-four-load
--param=system.cpu[0].numROBEntries=640
--param=system.cpu[0].numPhysIntRegs=512
--param=system.cpu[0].numPhysFloatRegs=384
--param=system.cpu[0].LQEntries=256
--param=system.cpu[0].SQEntries=128
--param=system.cpu[0].RARQEntries=192
--param=system.cpu[0].RAWQEntries=128
--param=system.cpu[0].branchPred.idealPredictionsPerCycle=2
--param=system.cpu[0].branchPred.idealNoOverrideBubbles=True
--param=system.cpu[0].idealInstructionFetch=True
--param=system.cpu[0].enableTwoFetch=True
```

普通SPEC `.zstd` checkpoint不加`--raw-cpt`。本地诊断使用同一normal-dedup REF、`--enable-mem-dedup`、`--warmup-insts-no-switch=20000000 --maxinsts=40000000`；最后完整块才是正式20M测量。自动采集及窗口/同CI核对见[scripts/run_final_tea.py](scripts/run_final_tea.py)。

当前配置用于新的热点和残余瓶颈分析。后续每次只放松一个机制；如果使用另一组参数，需要重新保存实际INI、命令和分数，不能继续沿用本配置的30.20846分。
