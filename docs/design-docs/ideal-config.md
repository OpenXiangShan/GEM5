# ideal.py：初步 KISS 上限探索配置

`configs/example/ideal.py` 复用 `kmhv3.py` 的系统构建、checkpoint 恢复和运行流程，只集中设置资源与前端差异。它用于同版本对照与后续实验，不继承历史 R10 28.72509 分的配置或测量口径。

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
| FP speculative wakeup | 开启 | 关闭：与本配置LVP组合的兼容性限制；FMUL/FMA执行延迟仍为主线3/4拍 |
| IEW→Fetch / Commit→Fetch / squash 宽度 | 4 / 4 / 8 | 2 / 1 / 32 |
| TAGE / MGSC / PHAST | 主线默认 | TAGE 各表 8192 组 × 8 路，增加 MGSC local/PC 门控，开启 PHAST |
| LVP / L1 LLDP / L2 CDP | 关闭 / 关闭 / 开启 | 主线 IdealConstantLVP / 开启 / 关闭 |
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

## 验证与性能记录

最终运行源码为`d379cf29cde13636c7c891806327a1f5044df65b`，已同步`xs-dev`至`14fcdbab13`。后续文档更新不改变被测运行代码。RISCV `gem5.opt`构建、默认/ideal CoreMark、namd短FP切片，以及PR的Clang构建、单测与smoke检查均通过；临时权限/边界探针不随PR提交。

| SPEC06 GCC16 RVA23 no-vector 0.3c | INT/GHz | FP/GHz | Overall/GHz |
| --- | ---: | ---: | ---: |
| 历史32 MiB L2、12宽，`61bdf46438` | 29.02525 | 27.44660 | 28.08914 |
| 2 MiB L2、10宽，合并主线前`0709985578` | 27.76761 | 26.80767 | 27.20079 |
| **最终2 MiB L2、10宽，`d379cf29cd`** | **27.76963** | **26.91932** | **27.26797** |

最终[run2078](https://github.com/OpenXiangShan/GEM5/actions/runs/42947459385)成功，145/145点正常完成、零abort、difftest通过；使用固定REF、checkpoint/list、权重及20M＋20M窗口，TEA关闭。每点实际INI精确匹配预注册的41项差异；使用归档自己的`cluster.json`重算，INT/FP/overall与CI一致。0.3c结果不作为1c分数。

历史到合并前新默认仅修改L2和四处宽度及文档，联合变化为INT −4.33%、FP −2.33%；不拆为L2单项收益。最终相对旧ideal为INT −4.33%、FP −1.92%；相对合并前新默认为INT +0.0073%、FP +0.4165%，还包含主线FP执行时序和resolve输入过滤等变化，不能归因于单个参数。TAGE及其余ideal资源参数保持。

FP speculative wakeup的关闭是兼容性约束：中间合并版本在`soplex_pds-50_9466`出现整数load difftest错误；同输入、REF与20M＋20M窗口的本地复现显示，单独关闭FP speculative wakeup或LVP均通过。最终保留constant LVP、关闭FP speculative wakeup，FMUL/FMA执行延迟仍为主线3/4拍；该切片在最终完整CI也通过。内部根因尚未确定，不能称为主线FP机制已修复。SMT、H-extension、自修改代码和完整RVV组合未纳入本次验收。
