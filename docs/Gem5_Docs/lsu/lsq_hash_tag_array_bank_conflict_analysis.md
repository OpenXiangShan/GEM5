# LSQ Hash Tag Array BankConflict Evaluation

## 目标

本实验比较三种降低 O3 load-pipe Dcache BankConflict 的策略，并记录
LSQ 内部 Hash Tag Array (HTA) 的建模边界。所有结果均使用
`gcc15-spec06-1.0c`、`kmhv3.py`、base vector、prefetch profile `off`，以及
同一个提交 `968a679e696b5b85d147e73079b78c2e420486dc`。

| 策略 | `EnableHashTagArray` | `DcacheBankBytes` | `DcacheSetDivNum` | `HashTagWidth` |
| --- | ---: | ---: | ---: | ---: |
| baseline | false | 8 | 1 | 8 |
| bank-2B | false | 2 | 1 | 8 |
| bank-4B-set-div2 | false | 4 | 2 | 8 |
| hash-tag-array | true | 8 | 1 | 8 |

## 模型与边界

HTA 由每个 CPU 的 `LSQ` 持有，因此同一 CPU 的 SMT threads 共用一份表。表的
set 数和 way 数从当前 L1D 几何自动推导；每项只保存 valid、security domain 和
folded tag，而不保存完整 Dcache tag。`HashTagWidth=8` 时，folded tag 是把 full
physical tag 连续切成 8-bit chunk 后逐段 XOR 的结果。

L1D refill 在 cache 已分配权威 VIPT set、way 和 full tag 后更新对应 HTA 项；
invalidation 会清空相同 set/way。首次从 demand packet 发现 LSQ owner 时，L1D
会回填已经驻留的 valid blocks，避免 restore/prefetch 预先填入的 cache line 永久
成为 HTA miss。lookup 使用 physical tag，并枚举 VIPT synonym sets。

BankConflict 的基线地址/bank 判断保持不变。HTA 仅在同周期 **load-load** 候选
上工作：两个 load 的 HTA way mask 没有重叠时，该候选不再造成 replay；任一
lookup miss、way mask 重叠、或 fake Dcache mainpipe 占用都保留原来的冲突。
多 bank load 只有在每个 touched bank 及其所有已有 load candidate 都通过该检查时
才整体放行。load-mainpipe 检测没有变化。禁用 HTA 时调用抽出的原始基线路径。

这是时序模型近似而不是 full-tag proof：folded-tag collision 可能保留本可避免的
冲突，也可能形成 false hit，从而放行 full-tag lookup 本应保留的候选（尤其目标
tag 未驻留时）。因此 HTA 的
`FilteredConflicts` 只能解释模型的放行次数，不能声称每次放行都证明了 full tag
对应不同 Dcache way。

## 验证

静态与构建检查：

```text
python3 -m py_compile src/cpu/o3/BaseO3CPU.py configs/common/LSQBankConflict.py configs/example/kmhv3.py
git diff --check
scons build/RISCV/gem5.opt --gold-linker -j64
```

以上检查均通过。CoreMark 使用最终二进制、8B bank、SetDiv=1 进行了开关 A/B：

| 模式 | IPC | `bankConflictTimes` | `BankConflictReplay` |
| --- | ---: | ---: | ---: |
| HTA off | 2.241830 | 7,916 | 7,875 |
| HTA on, width 8 | 2.236054 | 7,884 | 7,844 |

HTA-on 运行观察到 64 次 refill update、13,124 次 lookup、6,562 个 load-load
candidate、1,325 次 mainpipe 保留、6,559 次 way-overlap 保留和 3 次 filter。
这只证明 refill/lookup/filter 路径活跃；CoreMark IPC 不用于推导 SPEC 结论。

## CI 与统计口径

四个 `manual-perf` run 由同一新分支
`lsq-hash-tag-array-bank-conflict` 并发提交：baseline `30528903086`、bank-2B
`30528903002`、bank-4B-set-div2 `30528902992`、hash-tag-array `30528902988`。
每组目标为完整 1.0 coverage：55 个 workload、1,112 个 checkpoint slices，且
SPEC score 表包含 29 个 benchmark 行。

四个 CI 均已成功完成。每组都有 1,112 个 `stats.txt`、1,112 个完整 final-stat
段、零 abort 文件和完整 `score.txt`：

| 策略 | CI run | 归档 | 最终 stats | abort | coverage |
| --- | --- | --- | ---: | ---: | ---: |
| baseline | [30528903086](https://github.com/OpenXiangShan/GEM5/actions/runs/30528903086) | `20260730_181819_968a679e_kmhv3_run788` | 1,112 | 0 | 1.0 |
| bank-2B | [30528903002](https://github.com/OpenXiangShan/GEM5/actions/runs/30528903002) | `20260730_172230_968a679e_kmhv3_run787` | 1,112 | 0 | 1.0 |
| bank-4B-set-div2 | [30528902992](https://github.com/OpenXiangShan/GEM5/actions/runs/30528902992) | `20260730_202441_968a679e69_kmhv3_run786` | 1,112 | 0 | 1.0 |
| hash-tag-array | [30528902988](https://github.com/OpenXiangShan/GEM5/actions/runs/30528902988) | `20260730_192119_968a679e69_kmhv3_run785` | 1,112 | 0 | 1.0 |

总体性能和 benchmark score 直接采用 CI 生成的 `score.txt`。BankConflict 指标
从每个 slice 的最后一个 `Begin/End Simulation Statistics` 段读取：

- `system.cpu.lsq0.bankConflictTimes`
- `system.cpu.lsq0.loadReplayEvents::BankConflictReplay`
- HTA-on 的 `system.cpu.lsq.hashTagArray*`

对每个 workload，SimPoint counter 的 per-KInst 值严格按
`sum(weight * counter) / sum(weight * simInsts) * 1000` 计算；随后按 checkpoint
JSON 的 full-program dynamic instruction count 汇总到 suite。这样不会把不同 workload
的固定 ROI 原始计数直接相加。HTA probe/decision counters 位于更早的判断阶段，不能与最终
`bankConflictTimes` 或 replay counter 组成严格的加法分解。

## 结果

### Suite 级结果

下表的 score 直接来自各 run 的 CI `score.txt`。`IPC`、BankConflict 和 replay 由
完整最终 stats 按上节公式计算；括号内相对 baseline。score/IPC 越高越好，后两项
越低越好。

| 指标 | baseline | bank-2B | bank-4B-set-div2 | hash-tag-array |
| --- | ---: | ---: | ---: | ---: |
| Int score/GHz | 18.323022 | 18.358653 (+0.194%) | 18.535303 (+1.159%) | 18.369998 (+0.256%) |
| FP score/GHz | 20.711228 | 20.736236 (+0.121%) | 21.234550 (+2.527%) | 20.878611 (+0.808%) |
| Overall score/GHz | 19.687403 | 19.717179 (+0.151%) | 20.072955 (+1.958%) | 19.801487 (+0.579%) |
| Dynamic-instruction weighted IPC | 6.674857 | 6.685802 (+0.164%) | 6.822081 (+2.206%) | 6.715965 (+0.616%) |
| `bankConflictTimes` / KInst | 29.688664 | 28.055004 (-5.503%) | 12.578901 (-57.631%) | 22.985245 (-22.579%) |
| `BankConflictReplay` / KInst | 29.745104 | 28.118988 (-5.467%) | 12.630646 (-57.537%) | 23.063932 (-22.461%) |

按总体 score 排名是：bank-4B-set-div2 > hash-tag-array > bank-2B > baseline。
在本次 GEM5 性能口径下，`DcacheBankBytes=4` 且 `DcacheSetDivNum=2` 的组合同时
带来最大 BankConflict 降幅和最高 overall score；HTA 在保持 8B/SetDiv=1 时也有
明确收益，且优于仅把 bank byte 改为 2B 的方案。

### Benchmark 变化

下表列出每个策略相对 baseline 的代表性 benchmark score 变化。benchmark score
来自 `score.txt` 的三位小数行，适合识别主要收益/回退；suite score 使用上表中的
CI 原始高精度值。

| 策略 | 主要收益 | 主要回退 |
| --- | --- | --- |
| bank-2B | wrf +0.828%, h264ref +0.774%, omnetpp +0.670% | bzip2 -0.181%, milc -0.041%, soplex -0.039% |
| bank-4B-set-div2 | cactusADM +12.871%, gamess +10.494%, tonto +3.843%, wrf +3.595%, h264ref +3.040% | GemsFDTD -0.229%；其余最小变化项为正 |
| hash-tag-array | cactusADM +3.860%, gamess +1.887%, tonto +1.630%, h264ref +1.612%, wrf +1.349% | perlbench -0.713%, bzip2 -0.447%, gcc -0.213% |

BankConflict 的输入级变化也显示三种策略的覆盖范围不同：bank-2B 在
`bzip2_html` 从 14.182 降至 7.648 /KInst (-46.075%)，但 `mcf` 反而从 61.563
升至 61.635 /KInst (+0.116%)；bank-4B-set-div2 在 `bzip2_html`、
`bzip2_liberty`、`bzip2_combined`、`sjeng` 和 `bzip2_source` 上分别降低
75.950%、72.305%、69.234%、68.780% 和 68.659%；HTA 的最大降幅集中在
`hmmer_retro` (-60.403%)、`hmmer_nph3` (-60.071%)、`sphinx3` (-43.154%)、
`calculix` (-41.841%) 和 `bzip2_html` (-40.336%)。这些是单 input 的
profile-weighted rate；suite 级结论以表中的 full-program instruction weighting
为准。

### HTA 计数器

HTA-on 的计数器均为 suite 级 per-KInst：

| 计数器 | 值 / KInst |
| --- | ---: |
| `hashTagArrayRefillUpdates` | 6.928042 |
| `hashTagArrayInvalidations` | 6.895768 |
| `hashTagArrayLookups` | 19.528402 |
| `hashTagArrayLookupMisses` | 2.365360 |
| `hashTagArrayLoadLoadCandidates` | 9.891668 |
| `hashTagArrayMainPipeRetainedConflicts` | 19.117585 |
| `hashTagArrayNoHitRetainedConflicts` | 1.612386 |
| `hashTagArrayWayOverlapRetainedConflicts` | 2.394939 |
| `hashTagArrayFilteredConflicts` | 5.629409 |

HTA 相对 baseline 少了 6.703419 `bankConflictTimes` /KInst 和 6.681172
`BankConflictReplay` /KInst，量级与 5.629409 `FilteredConflicts` /KInst 一致，
支持该过滤路径确实影响了 BankConflict 压力。它们不是严格的等式：HTA decision
发生在后续 cache-port/timing 结果之前，且 multi-bank load 只计一次 filter。尤其是
19.117585 /KInst 的 mainpipe retain 表明 load-mainpipe 冲突仍被保留，符合本 feature
的作用域限制。

### 解释与结论

- 如果只比较这四个 GEM5 性能配置，bank-4B-set-div2 是最佳选择：overall score
  +1.958%，BankConflict replay -57.537%。该点同时改动了 bank byte 和 set div，
  本实验不能把收益严格拆分给两个参数中的任一个。
- HTA 在不改变 8B/SetDiv=1 基线银行组织时，将 overall score 提升 0.579%，
  BankConflict replay 降低 22.461%。这说明只过滤 load-load 的不同-way 候选已能
  带来可观收益，但仍低于改变 bank/set 组织的组合策略。
- 2B bank 的总体收益只有 +0.151%，其 suite 级 BankConflict replay 仅降低
  5.467%。因此在当前模型和 workload 集中，单独减小 `DcacheBankBytes` 的收益较小。
- 这份比较不包含 Dcache bank/set 变化的面积、能耗、时序或 RTL 实现成本。HTA 结果
  也只代表 `HashTagWidth=8` 的折叠 tag 近似；collision 和 false hit 的风险仍需要
  更宽 hash 的敏感性实验或 full-tag reference 对照来量化。

## 复现与审计

- 实现提交：`968a679e69` (`cpu-o3: Add LSQ hash tag array filter`)
- CI branch：`lsq-hash-tag-array-bank-conflict`
- 结果归档根目录：`/nfs/home/share/gem5_ci/performance_data/gcc15-spec06-1.0c/`
- profile JSON：`/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260604/json/checkpoints_all.json`
