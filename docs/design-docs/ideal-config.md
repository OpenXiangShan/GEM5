# ideal.py：初步 KISS 上限探索配置

`configs/example/ideal.py` 复用 `kmhv3.py` 的系统构建、checkpoint 恢复和运行流程，只集中设置资源与前端差异。它是方便后续实验的初步配置，性能分数需要重新测量，不是历史 R10 28.72509 分的原样复现。

## 复用与改动范围

- LVP 通过已有 `--enable-value-prediction` 路径启用主线的 `CompositeValuePredictor + IdealConstantLVP`，没有新增预测器实现。该模型按完整 PC 保存训练值，没有有限表容量和访问端口约束，但仍执行真实 load、校验值预测并处理错误恢复。
- scalar issue 延迟、constant folding 和共享 leaf TLB 容量沿用主线默认。短 issue 唤醒、folding 保护、TLB 扩容修正、除法时延修正均不在本次范围内。
- 新 C++ 支持集中在按需 functional Execute 指令供给、同周期串行双预测和取消 override 气泡；权限、canonical address、fault 和 squash 路径保留。

| 项目 | kmhv3.py 默认 | ideal.py 初步配置 |
| --- | --- | --- |
| decode / rename / commit / 物理寄存器释放 | 各 8 | 各 10 |
| 前端 | timing I-cache，默认预测带宽与 override 等待 | 按需 functional 供给、串行双预测、双目标取指、零 override 气泡 |
| scalar issue 延迟 / constant folding / shared leaf TLB | 2 周期 / 关闭 / 128 | 保持主线默认 |
| 六个整数 IQ 的 ready 新入队项 | 下周期可参与选择 | 同周期可参与选择 |
| load / store 地址端口 | 3 / 2 | 4 / 2，追加 ld3 及 wakeup 连接 |
| ROB / Int PRF / FP PRF | 352 / 224 / 256 | 640 / 512 / 384 |
| LQ / SQ / RAR / RAW | 120 / 64 / 96 / 56 | 256 / 128 / 192 / 128 |
| L1 DTLB / FP IQ | 48 / 每队列 18 项 | 512 / 每队列 24 项 |
| IEW→Fetch / Commit→Fetch / squash 宽度 | 4 / 4 / 8 | 2 / 1 / 32 |
| TAGE / MGSC / PHAST | 主线默认 | TAGE 各表 8192 组 × 8 路，增加 MGSC local/PC 门控，开启 PHAST |
| LVP / L1 LLDP / L2 CDP | 默认开关 | 主线 IdealConstantLVP / 开启 / 关闭 |
| L2 / L3 | 2 MiB / 32 MiB | 2 MiB（4 片各 512 KiB、data/dir 各 4 bank）/ 64 MiB |

“10 宽”指表中四处流水宽度，不额外施加全局每周期最多 10 条 issue 的硬限制。该配置仍使用实际分支预测器和恢复流程；functional 供给绕过普通 timing I-cache 等待。容量、吞吐和时延假设未包含面积、功耗与频率代价，测得差距用于寻找优化方向。

入口支持单核、单线程、四片 wrapper L2 和普通 DecoupledBPUWithBTB；SMT、SpMT、Hybrid ROB、Ruby、trace 和 ECore 不在范围内。现有 `idealkmhv3.py` 继续作为另一套独立配置使用。

## 同版本成对比较

先按常规方式准备 DRAMsim3 并构建，用同一 binary、输入、REF 和测量窗口依次运行两个入口：

```sh
scons build/RISCV/gem5.opt --gold-linker -j64
gem5_ref="$(python3 util/nemu_ref/resolve.py normal-dedup)"
for config in kmhv3 ideal; do
  GCBV_REF_SO="$gem5_ref" build/RISCV/gem5.opt \
    --outdir="/tmp/ideal-coremark/$config" "configs/example/$config.py" \
    --generic-rv-cpt=/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin \
    --raw-cpt --enable-mem-dedup --disable-tea \
    --warmup-insts-no-switch=0 --maxinsts=300000
done
```

普通 SPEC `.zstd` checkpoint 不加 `--raw-cpt`；替换输入并使用 `--warmup-insts-no-switch=20000000 --maxinsts=40000000`，即 20M 预热加 20M ROI。比较最后一个完整统计块的指令、周期与 IPC，先确认正常完成和 difftest 无错误。单切片结果不能代替加权 SPEC 整分；不同输入的内存大小和 REF 仍须匹配。

`--param` 在 preset 之后应用，例如 `--param='system.cpu[0].numROBEntries=352'`、`--param='system.cpu[0].idealInstructionFetch=False'`、`--param='system.cpu[0].branchPred.idealPredictionsPerCycle=1'`。修改 issue 延迟或共享 leaf TLB 几何不在本次验证范围内。

完整测量可在 `manual-perf.yml` 分别选择 `kmhv3.py` 和 `ideal.py`，固定同一 source ref、benchmark profile、REF 和参数；正式计分显式加 `--disable-tea`。新入口产物为 `score-upperbound-<benchmark_type>`，与 `score-*` 和既有 `score-ideal-*` 区分。0.3c 和 1c 各使用自己的采样点与权重。

## 验证与后续

历史 28.72509 来自源码 `36584adaf6` 的另一套配置。这里复用了不同的 LVP 并保留主线 issue/folding/leaf TLB 默认，因此不继承旧分数或旧逐点性能结果。下一轮应在同一新版本上重新测量默认与 ideal 的完整 profile。

2026-11-05：精简配置通过 RISCV `gem5.opt` 构建、CoreMark 默认/ideal/参数覆盖三次短程运行、gcc_expr/1687、mcf/6753、perlbench_checkspam/5886 三对 200k 预热加 800k ROI difftest，以及本地临时 Sv39/Sv48 权限与取指边界检查（测试程序不随 PR 提交）。INI 确认使用主线 CompositeValuePredictor/IdealConstantLVP、scalar issue=2、folding=False、shared leaf=128；mcf 与 perlbench 中观测到实际值预测。

默认入口在 CoreMark 300k 检查中的 5213 个既有 INI 字段、300001 条指令和 207449 周期与 base 一致。上述仅为配置和正确性 smoke，不是新配置的完整加权性能结果。

2026-11-10：默认 L2 总容量收回至 2 MiB（四片各 512 KiB，DRRIP 每片 1024 sets），decode/rename/commit/物理寄存器释放宽度设为 10。其余 preset 参数保持不变。此前 32 MiB L2、12 宽版本的 SPEC06 0.3c INT 29.02525 属于旧配置；新组合尚无完整加权跑分。CoreMark 300k difftest 通过：实际 INI 仅有预期的21处字段变化；与旧 preset 加等价参数覆盖的 8510 个非host统计值完全一致。
