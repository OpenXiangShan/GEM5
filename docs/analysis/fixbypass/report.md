# SDBP bypass 性能分析与修复

分析目录：`/tmp/fixbypass-analysis`；独立 worktree：`~/fixbypass`。
本文记录已完成的 8 个本地切片测量；排队中的全量 CI 不计为通过。

## 对照口径

| 变体 | GitHub run | 提交 | SPEC06 3 GHz 总分 |
|---|---|---|---:|
| SDBP bypass 开 | 43238615944 | 0fd6c64e9c | 47.332495 |
| SDBP bypass 关 | 42941526485 | 2e545b04af | 47.418754 |
| LRU | 42941521627 | 2e545b04af | 47.008429 |

三组均有 1094 个 `completed` 切片和对应统计，无缺失，score 的 coverage 均为 1.0。
checkpoint 清单、权重 JSON 的 SHA-256 三组完全一致，见 `input-validation.json`。
两次基线 Actions 的 failure 来自仿真、计分后的历史归档清理，错误为 NFS stale file handle；
不能因此把已完成的数据丢掉，也不能把它们的 workflow 状态写成 success。

固定 `idealkmhv3.py`、SPEC06 RVA23 novec GCC16 1.0c、20M 预热及 20M 测量、
`DRAMsim3`、exclusive L3、no-prefetch、L2 eviction invalidates L1、
`xiangshan_single_core` hierarchy policy、1 MiB/8-way/four-slice L2、32 MiB L3。
SDBP 固定 128x6 dead-first sampler、youngest=0、L1-copy protection。
REF 固定为：

```
/nfs/home/share/gem5_ci/ref/releases/d30fff1ece9e-gem5-r3-multi16g-zfa-cbo/normal-dedup/riscv64-nemu-interpreter-so
sha256 e5e58a35ae758b46de9c17cf8d30ef92e0a168d197939926a7185ee94a9315f3
```

本地基于开启 bypass 的提交构建。所有组均启用 difftest、mem-dedup，使用 checkpoint
内置 restorer。除了替换策略，非算法参数保持一致。原始二进制的 libquantum/85924、
bwaves/13153 开关两组，预热与测量两个 stats block 的所有非 host 计数器均精确复现原 CI。
`config.ini` 在重点切片的 on/off 差异只有四个 slice 的 `enable_bypass`；
DRAM ini 的不同绝对根路径指向同内容配置。原 on/off 提交不同带来的疑问由该复现排除。

## 性能与计数器

开启 bypass 相对关闭的总分变化为 -0.1819%，相对 LRU 为 +0.6894%。
benchmark 主要退化：bwaves -2.6185%、libquantum -1.9313%、milc -1.2124%。
完整 benchmark 结果见 `score-comparison.csv`。

| 切片 | bypass 开周期 | 关周期 | 周期增加 | 开/关 L2 misses | 开/关 L2 平均 miss latency |
|---|---:|---:|---:|---:|---:|
| libquantum/85924 | 5,303,213 | 4,689,674 | 13.083% | 511,767 / 511,707 | 13.571 / 9.820 ns |
| milc/7983 | 35,418,295 | 32,764,803 | 8.099% | 893,835 / 893,302 | 69.214 / 65.938 ns |
| bwaves/13153 | 18,314,583 | 17,707,825 | 3.426% | 750,863 / 751,454 | 83.606 / 80.417 ns |
| milc/5841 | 31,216,489 | 30,128,359 | 3.612% | 943,728 / 943,829 | 58.148 / 56.783 ns |
| libquantum/85656 | 5,055,401 | 4,812,330 | 5.051% | 499,230 / 501,248 | 12.634 / 12.058 ns |
| mcf/1820 | 37,368,914 | 33,623,530 | 11.139% | 1,726,997 / 1,754,766 | 83.374 / 75.693 ns |

bwaves/13153 权重 0.72016，是整体回退的重要来源。latency 用四 slice 的
`sum(missLatency)/sum(misses)` 计算，不能把 slice 平均值直接相加。
DRAMsim3 AbstractMemory 计数器在这里累计到测量结束，测量值需要减去预热 block；
其他已 reset 的缓存计数器取最后的测量 block。原始选点证据见 `point-counters.csv`。

L2 miss 数和 DRAM 读数基本不变，不能把这些回退主要归因为容量 miss 增加。
libquantum/85924 在开/关两边 `deadHits` 都为 0，关闭 bypass 时恰有大量可替换的
predicted-dead 行，说明这里首先应检查绕过的代价，而不是先调整 PC hash 或阈值。

L1 bank-conflict 次数：libquantum/85924 从 210,839 增到 361,258；bwaves/13153 从
217,153 增到 359,070；milc/7983 从 607,190 增到 673,571。
libquantum/85924 的 L3 总线 ReadSharedReq blocked attempts 从 145,276 增到 457,557，
WritebackDirty blocked attempts 从 31,272 增到 222,575。它们是阻塞/重试尝试数，不能当作独立请求数。
L3 总写回数基本不变：改变的是写回发起层级、时机及与 demand 的竞争。

## TopDown

优先使用模拟器原生 `system.cpu.*Bound`，不要直接使用当前处理工具重算的同名列。
工具的旧公式在 libquantum/85924 上得到 badSpecBound=-0.040366，而原生值为 0.000775；
部分层级不再相加闭合。`topdown-benchmarks-native.csv` 来自原生切片值，并按预估执行
周期加权聚合 benchmark；backend 用原生 retiring/frontend/badspec 的补数计算，仅有打印精度误差。
分数 CSV 则已与三组归档 `score.txt` 精确核对。

| 切片与原生指标 | bypass 关 | bypass 开 |
|---|---:|---:|
| bwaves/13153 memoryBound | 54.0342% | 55.4312% |
| bwaves/13153 l2Bound | 33.9234% | 35.5103% |
| milc/7983 memoryBound | 79.1618% | 80.7788% |
| milc/7983 l2Bound | 68.9089% | 71.3902% |
| libquantum/85924 coreBound | 29.8674% | 37.3215% |
| libquantum/85924 l3Bound | 1.0321% | 1.1343% |

libquantum 的 coreBound 是 scheduler 执行停顿减去被识别的 load/store 停顿后的剩余项，
不能解读为 ALU 运算变多。原生 exec_stall_cycle 从 1,553,516 增到 2,245,922；
load stalls 从 202,152 增到 222,041，store stalls 从 423,006 增到 510,440。
结合 bank replay 与 trace，结果支持数据回送、流水线竞争和调度连锁停顿。
TopDown 比例不是相互独立的根因证明，还需看绝对计数及请求链路。

## 请求、地址与 PC trace

原版每组仍运行完整 40M instructions，只在预热末 tick+1M 至其后 60M ticks 打印
`Cache,CacheRepl,SDBP,MSHR,L2CacheSlice,XBar`，不缩短计分区间。
原始压缩 trace 在 `/tmp/fixbypass-analysis/local/baseline/<on|off>/<point>/m5out/trace.gz`。
libquantum/85924 的 on/off 窗口分别观察到 17,912 次 bypass、20,312 次替换；
后者有 20,208 次选中 dead victim。

17,912 次 bypass 全来自 PC `0x12286`。匹配 checkpoint profile 的 libquantum.elf 可定位为：

```asm
quantum_cnot:
  12280: slli a4,a3,0x4
  12284: add  a4,a4,a1
  12286: ld   a5,8(a4)
  ...
  12290: sd   a6,8(a4)   # conditional store on another path
```

地址 `0x84b00000` 的实际事件：

| 事件 | bypass 开 tick |
|---|---:|
| L2 PC 0x12286 confidence=9，选择 bypass | 4,198,574,223 |
| L1 发出 WritebackClean，writable=1 dirty=0 | 4,201,914,879 |
| L2 不分配，转发 bypass writeback | 4,201,915,212 |
| L3 接受 WritebackClean miss | 4,201,917,210 |

关闭 bypass 时相同 PC/地址正常分配 L2，L1 在 tick 4,217,232,213 发出不带数据的
CleanEvict，稍后由 L2 返回数据到 exclusive LLC。不同策略此前已经改变 placement，
所以不能要求该地址的每次 LLC hit/miss 都相同。关键是 L1 是否需要额外读数据并回送。
摘要见 `libquantum85924-*-trace-summary.json`。

同样 60M ticks 内，on/off 的 L3 ReadSharedReq RETRY 为 114,587/33,983；
WritebackClean RETRY 为 2,420/98。窗口的请求数不同，应结合吞吐变化解读，不能把全部
重试归到某一个物理请求。bwaves 的已打印 PC 分布在 `MAIN__` 的加载序列中。

在匹配的 milc/7983 窗口，开启/关闭 bypass 的 L1 数据写回为 3,702/818 次，
而 L3 访问为 6,771/6,761 次；这支持传输层级变化，而非总内存请求大幅增加。
其 PC 包括 `0x1d4c2`、`0x1d4ac`、`0x1d4aa`。bwaves/13153 窗口的 L1 数据写回
为 1,756/704 次，WritebackClean RETRY 为 4,536/717 次。各窗口和重试尝试数的
完整摘要以 `milc7983-*-summary.json`、`bwaves13153-*-summary.json` 保存。

## 修复过程与行为

第一版只把 eligible dead-victim selection 放到 bypass 前。实际 libquantum/85924
周期只下降 0.958%，仍比关闭 bypass 慢约 12%；milc/7983 只下降 0.224%。该版本未作为
最终方案，也没有据此触发全量 CI。

一个 streaming PC 在新阶段开始时，旧缓存内容可能全是 live 标记。
旧实现此时不断 bypass，且 sequence 只在真实 touch/reset 时前进，绕过本身不会让被保护的
旧行变老。因而只调整 dead 优先级不能可靠打破旧 live 内容的长期保留。

第二版在保持 independent sampler、预测器、请求命令、数据与所有权传输不变的前提下：

1. 有 invalid way 时继续优先分配。
2. 有未受 L1-copy protection 保护的 dead victim 时，优先替换它。
3. 否则，预测为 dead 的 demand data refill 只保护近期 live victim；其年龄上限为
   `num_sets * candidate_ways` 个策略访问，即一个当前 L2 slice 容量。
4. bypass 自身推进 replacement sequence，避免没有真实插入时年龄冻结。
5. 年龄较大但仍在 L1 的 victim 继续受保护，避免把 L1-only 的热点误当成冷数据。
6. 新增 dead/stale 分配与 L1 保护计数器和带 PC/地址/年龄的 SDBP trace。

`--sdbp-bypass-dead-victims` 明确选择原始无界策略，供同二进制对照。
原有传输微测试显式选它，继续强制覆盖 clean/dirty bypass 链路；新策略单测验证默认准入、
纯 bypass 引用的老化、真正 reuse 刷新、L1 驻留保护。该改动没有额外调整 CPU/总线/存储器
参数，也没有根据 benchmark 名称作特判。

风险是有限时间窗口可能牺牲很长间隔的 L2 reuse；L1-copy 查询仍沿用既有理想 tag probe，
不能据此声称 RTL 的面积、能耗或探测开销已建模。必须通过正常参数的多切片和全量 CI 验证收益。

## 本地验证与后续全量 CI

31 个 SDBP interface 单测通过，包含 bypass 引用老化、真实 reuse 刷新及 L1 驻留保护。
两个配置测试通过，覆盖 320 组 classic/sliced、多核和新/原策略组合。
第二版不可变二进制通过既有三层 dirty responsibility/clean bypass 回归 32/32。
新编译的最终版本再次通过 interface/config 检查；style 与 `git diff --check` 通过。
原策略传输微测试继续强制覆盖 clean/dirty bypass，正常 checkpoint/difftest 运行覆盖新默认策略。

原版已有 10 组完整 checkpoint 开/关复现实验完成，预热和测量两个 block 的非 host 统计差异均为 0。
第二版所有 8 组生成配置与原 on CI 对比，只有四个 slice 的新算法选项
`bypass_dead_victims=false` 和内容相同的 DRAM ini 根路径不同。
精确 argv、固定 REF 与开始时间见 `local-commands.json`，配置差异见 `bounded-config-validation.json`。
`local-completion-audit.json` 逐组确认退出码、两个指令区间、checkpoint、REF 哈希、
所有非算法命令行参数及 DRAM 配置文件内容相同。

下表只列已完成的第二版测量。周期负变化表示加速，正变化表示回退。

| 切片 | 第二版 cycles | 相对原 bypass 周期变化 | 相对关闭 bypass | 测量 bypass 次数 |
|---|---:|---:|---:|---:|
| libquantum_85924 | 4,707,901 | -11.225% | +0.389% | 5 |
| libquantum_85656 | 4,796,882 | -5.114% | -0.321% | 41 |
| bwaves_13153 | 17,706,707 | -3.319% | -0.006% | 775 |
| milc_7983 | 32,778,781 | -7.452% | +0.043% | 2,422 |
| milc_5841 | 30,199,381 | -3.258% | +0.236% | 3,257 |
| mcf_1820 | 33,607,662 | -10.065% | -0.047% | 658 |
| sphinx3_157921 | 5,732,835 | +0.993% | -0.127% | 1,836 |
| xalancbmk_10335 | 7,453,472 | +3.766% | -0.342% | 741 |

8 个切片均完成完整的 20M 预热及 20M 测量，difftest 运行退出码均为 0。
6 个重灾点周期均下降，两个原 bypass 收益控制点回退。
8 个完整 SDBP trace 的测量区间事件数与对应 bypass/dead-allocation/stale-allocation
计数器逐项相等，见 `bounded-policy-summary.json`。

libquantum/85924 的 L2 平均 miss latency 从 13.571 降到 9.861 ns；bank conflict 从
361,258 降到 211,413，coreBound 从 37.3215% 降到 29.6216%。
这与绕过导致的数据回送/资源竞争及旧 live 内容保护假设相符，不能从单点外推全套分数。
同一个第二版二进制用 `--sdbp-bypass-dead-victims` 恢复原策略时，libquantum/85924
精确得到原 CI 的 5,303,213 cycles；预热/测量的全部已有非 host 计数器差异为零。
新增诊断计数器单独列出，不混入该一致性断言，见 `same-binary-original-verification.json`。
sphinx3 控制点的收益损失必须保留：优先替换与有限保护也可能减少原 bypass 的收益。

mcf/1820 的原版 on/off trace 定位到 PC `0x113cc`（`ld a3,8(a5)`）和
`0x113da`（`ld a3,16(a5)`），位于每次推进 64B 且连续执行 store 的循环中。
地址 `0x890b1e80` 在 L2 bypass 后，以 `WritebackDirty` 从 L1 经 L2 回到 L3。
这里主要是 dirty 流量，不能只用额外 clean 数据回送解释所有退化。
修复版的 L2 平均 miss latency 从 83.374 降到 75.588 ns，
测量 bypass 从 1,667,179 降到 658。其第一版只调整 dead 优先级的周期减少 0.021%，
加入年龄上限后的周期减少 10.065%，支持必须处理旧 live 内容长期保留这一判断。
mcf 的 bank-conflict 计数同时从 1,990,066 增到 2,238,592；这说明它的加速不能
用 bank conflict 单项下降解释，应结合 L2 miss latency、写回时机及总停顿判断。

已触发全量 1.0c：GitHub Actions run **43282989822**，被测提交
`56aceca0ba8398a2526eaca7ded59e3921216fd3`。setup 成功，确认时性能 job 为 queued，
没有最终 conclusion，不能写成 CI 已通过。`extra_args` 与 run 43238615944 的
`resolved_extra_args` 逐字符相同，REF 路径固定，全部 benchmark、distributed=default、
每机 32 jobs。完整输入和核对记录见 `ci-dispatch.json`。
构建及测试日志、原始 trace、统计和退出记录在 `/tmp/fixbypass-analysis`。
