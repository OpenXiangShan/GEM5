# SDBP：香山 ideal KMHv3 验证记录

## 结论与性能结果

18/18 组正式实验均正常完成，difftest 没有报错。SDBP 在实际香山 L2
替换链路中生效，但当前参数没有改善这 6 个切片的总体性能：
非加权几何平均 IPC 比值为 SDBP **0.998677（-0.132%）**，
SDBP+bypass **0.997905（-0.209%）**。这是指定切片的子集统计，
不是 SPEC 加权总分，也不是对全部工作负载的结论。

| 切片 | LRU IPC | SDBP IPC | SDBP 相对 LRU | SDBP+bypass IPC | bypass 相对 LRU |
| --- | ---: | ---: | ---: | ---: | ---: |
| mcf/12886 | 0.728984 | 0.727250 | -0.238% | 0.734151 | +0.709% |
| libquantum/27811 | 5.798421 | 5.800463 | +0.035% | 5.800463 | +0.035% |
| lbm/6382 | 2.491074 | 2.494594 | +0.141% | 2.495371 | +0.173% |
| hmmer_nph3/7894 | 7.006453 | 7.006453 | +0.000% | 7.006453 | +0.000% |
| omnetpp/18492 | 1.520350 | 1.510357 | -0.657% | 1.494162 | -1.723% |
| astar_biglakes/5863 | 4.796209 | 4.792710 | -0.073% | 4.775411 | -0.434% |

测量阶段机制计数如下。`nonLruDeadVictims` 证明选择确实偏离 LRU；
它与 bypass 数是分配尝试计数，不是去重的完成事务数。

| 切片 | SDBP 非 LRU dead victims | bypass 组非 LRU dead victims | bypass 次数 |
| --- | ---: | ---: | ---: |
| mcf/12886 | 29,970 | 16,086 | 366,280 |
| libquantum/27811 | 42 | 42 | 0 |
| lbm/6382 | 9,285 | 9,254 | 0 |
| hmmer_nph3/7894 | 0 | 0 | 0 |
| omnetpp/18492 | 30,386 | 19,529 | 55,458 |
| astar_biglakes/5863 | 199 | 263 | 153 |

所有策略统计都通过以下一致性检查：访问分类之和等于 lookups，
liveTraining 等于 samplerHits，deadTraining 等于 samplerEvictions，
nonLruDeadVictims 不大于 deadVictims。更详细的数值见同目录
`results-20260924.csv` 与原始输出目录的 `comparison.json`。

当前证据支持保持显式 opt-in 和默认关闭 bypass。后续若要优化，优先检查
PC 覆盖、sampler 相联度/阈值、L1 预取与 mostly-exclusive L3 的交互；
本轮没有根据测试结果回调参数，也没有选择性舍弃负向切片。

## 实验口径

实验日期为 2026-09-24，主机为 `node040`。实现分支为 `codex/sdbp`，
从 `origin/xs-dev` 的 `942ef54e9e32125d693a8a1abce0a819e0263079`
创建；实现提交为 `cbfbf8695d09251664543ed2e927c7c366ff0b83`。

正式性能实验仅采用 `configs/example/idealkmhv3.py` 的香山单核
full-system 配置。此前使用 `kmhv3.py` 的尝试已经取消，其数据不计入结果。
通用 TimingSimpleCPU 小程序只用于功能回归，不用于性能结论。

共同条件：

- 每个 checkpoint 预热 20M 条指令，随后统计 20M 条指令；不切换 CPU。
- 三组分别为 L2 LRU、L2 SDBP、L2 SDBP+bypass；L3 固定 LRU。
  L1 沿用香山配置，包括 DCache 的 TreePLRU。
- L2 为 2 MiB、8-way、4 slices；每 slice 512 KiB、1024 sets。
- SDBP 每 slice 有 32 个 6-way sampler sets，三张 4096-entry、2-bit
  预测表；dead 阈值为 8，partial tag/signature 各 15 bit。
- 默认 XOR folding PC hash，`pc_shift=1`，各表独立 mixed hash。
  训练仅使用带 PC 的 demand lookup，过滤预取和写回。
- 每 slice 独立预测器，即总计 128 个 sampler sets、12 张预测表。
  该资源配置不等于论文中的单个 32-set predictor。
- 预取器、CPU、内存及其他配置不变；汇总脚本检查 `config.json`，
  只允许被测层 replacement policy 节点不同。
- 全部开启 difftest 和 mem-dedup。普通 GCPT 使用内嵌 restorer，
  没有 `--raw-cpt` 或外部 restorer 覆盖。

被测二进制 SHA256：

```text
dc9f4b9473f2fc24a1e434e9207ecc49fea68919004a71536c3816ad4cca38aa
```

参考模型由 `python3 util/nemu_ref/resolve.py normal-dedup` 选择，版本
`d30fff1ece9e-gem5-r3-multi16g-zfa-cbo`，SHA256：

```text
e5e58a35ae758b46de9c17cf8d30ef92e0a168d197939926a7185ee94a9315f3
```

## 复现命令与证据位置

在仓库根目录执行：

```sh
python3 util/sdbp_reference/run_comparison.py \
  --output=m5out/sdbp-ideal-spec06-20260924 \
  --config=configs/example/idealkmhv3.py \
  --checkpoint-root=/nfs/home/share/checkpoints_profiles/spec06_gcc16_rva23_novec_260820/checkpoint \
  --checkpoint=mcf/12886 \
  --checkpoint=libquantum/27811 \
  --checkpoint=lbm/6382 \
  --checkpoint=hmmer_nph3/7894 \
  --checkpoint=omnetpp/18492 \
  --checkpoint=astar_biglakes/5863 \
  --ref-so=/nfs/home/share/gem5_ci/ref/releases/d30fff1ece9e-gem5-r3-multi16g-zfa-cbo/normal-dedup/riscv64-nemu-interpreter-so \
  --extra-arg=--enable-mem-dedup --jobs=6
python3 util/sdbp_reference/summarize.py \
  m5out/sdbp-ideal-spec06-20260924
```

再次运行需选择新输出目录，脚本拒绝覆盖已有结果。机器相关的路径需按环境调整。

原始输出在仓库下 `m5out/sdbp-ideal-spec06-20260924/`：每组保存命令、
日志、配置、stats 和正常退出状态。`manifest.json` 保存输入和哈希；
`complete_implementation.patch`、`implementation_commit.txt` 与
`source_snapshot.tar.gz` 保存完整实现来源。初次启动实验时的
`tracked_changes.patch` 不含当时未跟踪的新文件，应使用完整实现归档。

手动 SIGUSR1 曾触发统计 dump，但没有 reset；汇总取最后一个完整统计区间，
不把这些快照相加。`simInsts` 必须在目标测量指令数的 100 条以内，并检查
正常到达 max instruction count 的退出原因。

## 正确性与机制验证

- `gem5.opt` 编译通过。
- 13 项 SDBPCore 单元测试通过，覆盖采样映射、训练、饱和、哈希、
  partial tag alias、安全域隔离、LRU 与非法参数。
- 同一组 13 项核心测试通过 AddressSanitizer 和 UndefinedBehaviorSanitizer。
- 6 项真实 replacement-policy 接口测试通过，覆盖响应 PC、fill 不训练、
  request/response 预取过滤、无 PC、dead 覆盖 LRU、bypass 和 invalidate。
- 小型读写 checksum 测试在 LRU、SDBP、SDBP+bypass 和 threshold=0
  情况下通过；MemTest 验证 15,682 次无 PC 请求没有训练、预测或 bypass。
- num_sets 不匹配、阈值越界、VIPT 和 skewed indexing 均在启动时拒绝。
- 香山 ideal 的 L3 SDBP 短程启动与 100K 指令 difftest 通过。
- gem5 style、clang-format、Black、Python 编译和 `git diff --check` 通过。

测试日志与功能输出位于主实验目录的 `evidence/`。

`evidence/ideal-trace-window/` 为实际香山 ideal 配置的短 mcf trace：
预热 100K、总共运行 1M 条指令，difftest 通过；只打印 tick
500,000,000 至 510,000,000 的事件。5,213 行 trace 中有 89 个 sampler
事件、2,236 个 victim 事件，其中 18 个选择了与 LRU 不同的 dead block。

```text
500002164: system.l2_wrappers.slices3.inner_cache.replacement_policy: victim set=751 way=6 dead=1 lru_way=7
500003829: system.l2_wrappers.slices3.inner_cache.replacement_policy: predict addr=0xcb9497c0 pc=0x108d2 signature=0x468 confidence=9 dead=1
```

这证明预测参与了替换选择，不等于证明所有 dead 预测正确。

## 机制分析

### libquantum：命中率变化大，性能收益小

LRU 到 SDBP 的 L2 demand misses 从 37,596 降至 4,989，其中 CPU data
从 1,895 降至 36，L1 预取 requestor 从 34,521 降至 3,767。
CPU data MSHR misses 从 1,198 降至 26；对应的 L2-miss stall 指标
从 7,463 周期降至 239 周期。SDBP 有 42 次非 LRU dead victim 选择。

但 LRU 的这一 stall 指标仅占 3,449,213 总周期的一小部分；最终 IPC
只提升约 0.035%。内存读总数从 525,574 变为 525,617，基本不变。
因此不能用 L2 miss 的大幅下降代替端到端性能结论。
SDBP+bypass 与 SDBP 完全一致，没有实际 bypass。

### mcf：单独替换回退，bypass 改善 IPC 并增加流量

SDBP 发生 29,970 次非 LRU dead victim 选择，CPU data misses 从
837,644 增至 838,831，L3 demand misses 从 1,219,308 增至 1,232,992。
L2-miss stall 指标从 9,165,837 增至 9,184,400 周期，IPC 回退约 0.238%。

开启 bypass 后，记录 366,280 次 bypass 和 16,086 次非 LRU dead victim。
CPU data misses 降至 817,097，CPU data MSHR misses 从基线 687,537
降至 668,909，L2-miss stall 降至 8,965,309 周期；IPC 比 LRU 提升约
0.709%。但 L3 demand misses 增至 1,354,622，内存读总数从 2,629,146
增至 2,994,817（约 +13.9%）。这些计数支持“关键停顿减少但下层流量
增加”的解释，尚不足以证明在带宽竞争更强的多核场景中也会受益。

### lbm：变化较小，PC 覆盖是明显限制

SDBP 的 IPC 提升约 0.141%，SDBP+bypass 约 0.173%。CPU data misses
分别为 LRU 645,827、SDBP 645,830、bypass 645,811，几乎不变。
SDBP 的 2,801,953 次 tag lookup 中只有 177,184 次符合 PC 训练条件，
有 1,183,827 次无 PC demand lookup。故 CPU requestor 的 miss 数量
与 `eligibleMisses=37` 差异很大，不能把 eligible 子集当成所有 demand。

SDBP 记录 9,285 次非 LRU dead victim。bypass 组在测量阶段的 bypass
为 0，但预热阶段有 2 次，因此它和 SDBP 组进入测量时的微架构状态可以
不同。微小的 IPC 差异不应解释为测量阶段发生了大量 bypass。

### hmmer：未触发 dead 预测，性能不变

三组均为 2,854,511 周期，IPC 7.006453，L2 demand misses 642。
SDBP 测量阶段有 309 次 sampler 访问、291 次 sampler hit，
`deadTraining=0`、`deadPredictions=0`、`nonLruDeadVictims=0`。
本切片没有足够 sampler 驱逐来形成 dead 预测，结果与 LRU 一致。

### omnetpp：bypass 的主要回退项

LRU、SDBP、bypass 的 L2 demand misses 为 470,467、468,603、470,042，
CPU data misses 为 193,655、193,125、193,277，均没有明显增长。
但 L3 demand misses 从 338,711 增至 345,495/358,916，内存读总数
从 879,591 增至 896,290/931,170。bypass 组有 55,458 次绕过，
其 L2-miss stall 从基线 789,128 增至 811,767 周期，
全部未完成 load 对应的 stall 从 5,022,597 增至 5,251,580 周期。
SDBP 与 bypass 的 IPC 分别回退 0.657% 和 1.723%。

这里的下层流量增长与停顿上升和回退方向一致，但现有计数不能把每一次
额外停顿映射到某个 bypass PC；“被绕过块仍有后续重用”是后续需要
按地址/PC 做 shadow-cache 或 trace 检验的假设，而非已证明的唯一根因。

### astar：total miss 略降，但 CPU data miss 上升且 IPC 回退

LRU、SDBP、bypass 的 CPU data misses 分别为 5,234、5,429、5,487，
L3 demand misses 为 15,434、16,209、16,183。SDBP 与 bypass 的非 LRU
dead victim 分别为 199 和 263，bypass 另外发生 153 次绕过。
IPC 相对 LRU 分别回退约 0.073% 和 0.434%。虽然 total L2 demand
misses 从 15,233 略降至 15,148/15,144，CPU 侧和端到端指标并未改善。
目前证据只支持工作负载相关的负收益，不能仅凭 sampler 的少量
`samplerDeadHits`（4/5 次）定位全部错误驱逐。

## 排序诊断与边界

原生 LRU 以 tick 记录 recency，SDBP 按访问序号记录，故同 tick 的访问
可能有不同次序。为检查这一因素，libquantum 额外运行了一组 ideal 配置：
SDBP 的 sampler 设置为 2 sets、32768 ways，保持阈值 8、关闭 bypass。
该超大 sampler 仅用于诊断，不是实用硬件设计。

预热和测量区间的 `deadTraining`、`deadPredictions` 均为 0，
因此真实 cache 始终按访问序号 LRU 替换。其测量周期 3,449,213、
L2 demand misses 37,596、CPU 数据 misses 1,895 与原生 LRU 完全一致。
而正常 SDBP 为 3,448,001 周期、4,989 个 L2 demand misses、36 个
CPU 数据 misses。这排除了该切片中 recency 次序差异的解释；未将此结论
外推到所有其他切片。诊断原始记录保存在 `evidence/sequence-control/`。

计数器解释与范围限制：

- `demandMisses::total` 在此分支也包含预取器 requestor 转为普通读请求
  后的 miss，不能等同于 CPU load miss。CSV 分列 CPU data、CPU data
  MSHR miss 和 L1 预取 requestor 的 miss。
- `samplerDeadHits` 仅表示 sampler 标签生命周期中观察到的错误 dead
  预测。提前驱逐后的再次访问无法由 `deadHits` 看见，不能由这两个值
  推导完整的 oracle precision/recall。
- `deadVictims`、`nonLruDeadVictims` 和 `bypasses` 记录选择/分配尝试，
  可能受后续 transient 状态重试影响，并非去重后的成功驱逐数。
- Store-buffer 合并请求在 `LSQ::SbufferRequest::addReq()` 中没有保存
  单条指令 PC；页表等请求也可能无 PC。本实现保守回退，不构造虚假 PC。
- 这是功能级替换模型，没有额外 predictor SRAM 延迟、端口竞争或能耗模型。
  predictor 和 sampler 不做 checkpoint 序列化，恢复后通过预热重建。
- 本轮只验证香山单核和指定 6 个切片，不覆盖 SMT、多核 coherence 的全部
  bypass 路径，也不是加权 SPEC 总分或论文性能复现。
- `idealkmhv3.py --classic-l2` 的短程检查遇到基线已有的 `slice_num`
  参数不存在错误，发生在 SDBP 配置之前。该入口没有通过集成验证；
  正式实验使用默认 sliced L2，不受影响。没有在本任务中修改此无关问题。

远端 manual-perf 输入与触发方式已检查并记录在 [README.md](README.md)。
本轮选择本地执行，没有 push 分支或 dispatch GitHub Actions，不能称作 CI 通过。
