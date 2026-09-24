# Sampling Dead Block Prediction 参考源码

本目录包含 JWAC-1 Cache Replacement Championship 的摘要、实现说明和验证脚本，参考原始实现：

> Dead Block Replacement and Bypass with a Sampling Predictor
> Daniel A. Jimenez，2010

源码压缩包：<https://www.jilp.org/jwac-1/online/code/001_jiminez.tgz>
程序页面：<https://jilp.org/jwac-1/JWAC-1%20Program.htm>
论文：<https://www.jilp.org/jwac-1/online/papers/001_jiminez.pdf>

原始源码压缩包使用 JWAC/CRC 接口，不参与 GEM5 构建。
新实现位于 `src/mem/cache/replacement_policies/sdbp_{core,rp}.{hh,cc}`，
通过 `SDBPRP` 配置。它采用下述参数化设计，不是比赛源码的逐行移植。

已完成的香山 `idealkmhv3.py` 六切片 LRU/SDBP 对照、计数器分析与验证记录
见 [RESULTS.md](RESULTS.md)，完整数值见
[results-20260924.csv](results-20260924.csv)。

原始源码允许复制、修改和再发布，具体版权说明见上述压缩包中的 `replacement_state.cpp`
和 `replacement_state.h`。

## 与 `SDBP.pdf` 的对比

GEM5 仓库中的 `SDBP.pdf` 是论文 *Sampling Dead Block Prediction for
Last-Level Caches*。JWAC 源码使用的是同一个 SDBP 算法，但它适配了 Cache
Replacement Championship 的接口。主要差异如下。

| 项目 | 论文配置 | JWAC 参考源码 | 结论 |
| --- | --- | --- | --- |
| Sampler 集合数 | 32 个集合；在 2,048 个 cache 集合中每隔 64 个采样 | 根据可用 bit 数动态计算；`sampler_modulus = nsets / nsampler_sets` | 配置方式不同 |
| Sampler 相联度 | 12-way | 默认 12-way | 默认一致 |
| 4,096 集合的 cache | 论文说明多核场景仍使用相同的 32-set sampler | 特殊处理改为 13-way sampler，并使用 14 位 predictor index | 源码包含比赛调参 |
| Partial tag | 15 bit | 16 bit（`dan_sampler_tag_bits`） | 不同 |
| PC/trace signature | 15 bit | PC 低 16 bit（`dan_sampler_trace_bits`） | 不同 |
| Predictor 表 | 3 张表，每张 4,096 个 2-bit counter | 3 张表，每张 `1 << 12` 个 2-bit counter | 默认大小一致 |
| Dead 阈值 | 置信度总和达到 8 | `dan_threshold = 8` | 一致 |
| Signature 来源 | 最近一次访问该 block 的指令 PC | 保存最近一次访问 PC 的低位；下次命中或驱逐时训练 | 来源一致，位宽不同 |
| Sampler 命中训练 | 将 sampled block 视为 live，降低置信度 | `block_is_dead(..., false)` | 一致 |
| Sampler 驱逐训练 | 将 sampled block 视为 dead，提高置信度 | `block_is_dead(..., true)` | 一致 |
| Victim 选择 | 优先选择预测为 dead 的 block，否则使用基准策略 | `Get_Sampler_Victim()` 优先选择 predicted-dead block，否则使用 LRU | 对默认 LRU 基准一致 |
| Bypass | 新 block 被预测为 dead-on-arrival 时绕过 cache | `Get_Sampler_Victim()` 返回 `-1` | 一致 |
| Predictor 组织 | 3 张使用不同 hash 的 skewed 表 | `f1`、`f2`、`fi` 生成 3 个表索引 | 一致 |
| Writeback | 论文没有重点展开 | 源码不使用 writeback 访问训练 sampler | 源码额外策略 |

如果 GEM5 实现的目标是复现论文结果，建议使用论文配置，而不是源码中的
比赛特定默认值：

```text
sampler 集合数       = 32
sampler 相联度       = 12
partial tag 位数     = 15
partial PC 位数      = 15
predictor 表数量     = 3
每张表的 entry 数    = 4096
counter 位宽         = 2
dead 阈值            = 8
```

论文机制与源码函数的对应关系如下：

- sampler 更新：`replacement_state.cpp` 中的 `UpdateSampler()`；
- victim 选择和 bypass：`Get_Sampler_Victim()`；
- sampler 训练：`sampler::access()`；
- skewed predictor 索引：`predictor::get_table_index()`；
- live/dead counter 训练：`predictor::block_is_dead()`；
- dead 预测：`predictor::get_prediction()`。

上述函数名指 JWAC/CRC 参考实现，新实现的接口见下文。

## 参数化设计建议

如果将 SDBP 移植到 GEM5，建议不要把论文或 JWAC 源码中的数值写死，而是
把下面这些项目做成 replacement policy 参数：

| 参数 | 含义 | 论文参考值 |
| --- | --- | ---: |
| `samplerNum` | 目标 sampler 集合数 | 32 |
| `samplerAssoc` | 每个 sampler 集合的 way 数 | 12 |
| `predictorTables` | skewed predictor 的表数量 | 3 |
| `predictorEntries` | 每张 predictor 表的 entry 数 | 4096 |
| `counterBits` | 每个饱和计数器的位宽 | 2 |
| `deadThreshold` | 预测为 dead 所需的置信度阈值 | 8 |
| `partialTagBits` | sampler 保存的 tag 位数 | 15 |
| `partialPcBits` | sampler 保存的 PC/signature 位数 | 15 |

预测表寻址还必须支持以下参数。这些是移植设计要求，不是论文给出的默认配置：

| 参数 | 含义 |
| --- | --- |
| `pcHashType` | PC signature 哈希算法，例如 XOR 折叠或混合哈希 |
| `pcHashSeed` | PC signature 哈希种子 |
| `pcShift` | 哈希前移除的 PC 低位数；例如 RISC-V 含压缩指令时可设为 1 |
| `indexHashType` | 从 signature 生成各预测表索引的哈希算法，可提供 JWAC 兼容选项 |
| `tableHashSeeds` | 每张预测表的哈希种子，数量必须等于 `predictorTables` |

### 使用可配置的 PC 哈希寻址预测表

新实现应对 PC 做哈希，不能仅截取 PC 的低位作为 signature。推荐区分
“生成可保存的 signature”和“生成各表索引”两个步骤：

```text
signature = pcHash(PC >> pcShift, pcHashSeed, partialPcBits)
index[t] = indexHash(signature, t, tableHashSeeds[t]) & (predictorEntries - 1)
```

`pcHash` 应混合输入 PC 的高低位，再输出 `partialPcBits` 位 signature；
不能在哈希前只保留低 `partialPcBits` 位，否则高位信息已丢失。
上式要求 `predictorEntries` 为不小于 2 的 2 的幂，索引宽度为
`log2(predictorEntries)`。各表必须使用不同的哈希映射，以保留 skewed
predictor 减少冲突的作用。

sampler 保存最近一次访问生成的 signature。再次命中或驱逐时，使用保存的
旧 signature 训练预测表；随后才用当前访问 PC 生成的新 signature 更新
sampler。预测查询和训练必须共用同一个索引函数及参数，不能查询时哈希完整
PC、训练时却改用截断 PC。若索引还混入线程标识，sampler 也需要保存对应的
线程信息，以便重建原索引。

在 GEM5 中，哈希算法可用枚举参数选择，种子使用整数或整数向量参数，并在
C++ 中统一实现哈希函数；RTL 对应实现可以使用 elaboration 时传入的函数。
哈希配置在一次仿真内保持不变，并记录到配置输出中，便于复现实验。
若需要复现 JWAC 原源码，可单独提供低位截取 signature 的兼容模式；它不应
替代新设计默认要求的 PC 哈希模式。

其中 `samplerAssoc` 不必等于 L2 的相联度。对于 8-way L2，可以先尝试
6-way（接近论文中 16-way L2 对应 12-way sampler 的比例），并将 4-way、6-way
和 8-way 作为可比较的实验点。

## 采样集合的选择

不建议使用固定步长，例如 `set % 64 == 0`。固定步长会和某些访问 stride
产生周期性对齐，导致采样集合长期观察不到该访问模式。采样集合应由 set
index 的位模式决定，使采样分布覆盖整个 set 空间，而不是依赖一个简单的
算术步长。

推荐使用下面的位段匹配条件：

```scala
val half_setBits = xxx
val match_a = set_s3(setBits - 1, half_setBits) ===
  set_s3(setBits - half_setBits - 1, 0)
```

这里的 `set_s3` 是完整的 set index，`setBits` 是 set index 位数。该条件要求
高位字段和低位字段相等；字段允许重叠。令 `matchBits = setBits - half_setBits`，
则采样比例恰为 `1 / 2^matchBits`。这种选择减少固定模数采样与 stride
直接对齐的风险，但仍是确定性的结构化采样，不能保证覆盖所有 stride。

### `half_setBits` 的计算

若 `samplerNum` 表示期望的 sampler 集合数，L2 一共有 `numSets = 2^setBits`
个集合，则推荐先按采样比例计算：

```text
targetRatio = numSets / samplerNum
matchBits = round(log2(targetRatio))
half_setBits = setBits - matchBits
```

上式的对数取整只是比例估算，未必使集合数量的绝对误差最小。
工程实现枚举合法候选值，选择实际采样集合数最接近 `samplerNum` 的值：

```text
half_setBits = argmin_h(abs(2^h - samplerNum)), 1 <= h < setBits
matchBits = setBits - half_setBits
actualSamplerNum = 1 << half_setBits
sampleIndex = setIndex & (actualSamplerNum - 1)  // only for sampled sets
```

如果 `numSets / samplerNum` 不是 2 的幂，不能同时精确满足目标数量和上述
位匹配形式。此时可以在
`floor(log2(numSets / samplerNum))` 与 `ceil(log2(numSets / samplerNum))`
之间选择，使 `actualSamplerNum` 与目标 `samplerNum` 的绝对误差最小；若两者
距离相同，建议选择较大的 `matchBits`（也就是较小的 `half_setBits`），
以减少 sampler 存储和训练流量。

例如，`setBits = 12`、`numSets = 4096`、`samplerNum = 32` 时：

```text
matchBits = round(log2(4096 / 32)) = 7
half_setBits = 12 - 7 = 5
actualSamplerNum = 4096 >> 7 = 32
```

此时采样条件为 `set_s3(11, 5) === set_s3(6, 0)`，即高 7 位与低 7 位相等。
由于两个字段重叠，得到恰好 `1 / 128` 的采样比例，并且采样位置由 set index
的位模式决定，不是每隔固定 64 个集合取一个样本。

实现时，MainPipe、sampler SRAM 和训练逻辑必须调用同一个采样判定函数，避免
不同模块对 `half_setBits` 或位段范围使用不一致的定义。

## GEM5 初版实现

### 配置与范围

在已有 cache 配置中显式指定替换策略，例如 2048-set、8-way 的 cache：

```python
from m5.objects import SDBPRP

cache.replacement_policy = SDBPRP(
    num_sets=2048,       # 必须等于 size / (assoc * cache_line_size)
    sampler_num=32,
    sampler_assoc=6,     # 实验选择，不代表 8-way 的最优值
    pc_hash_type="xor_fold",
    index_hash_type="mixed",
    enable_bypass=False,
)
```

目前仅支持 Classic cache 的 `BaseSetAssoc`，仅接受 `SetAssociative` indexing（包括其 slice 参数）；不支持 VIPT、skewed indexing、压缩/sector tag store、Ruby，
也不支持套在 DuelingRP 内。未修改任何现有配置的默认替换策略。

文档的 camelCase 参数对应 Python 的 snake_case 参数。新增参数包括
`num_sets`、`crc_live_update` 和 `enable_bypass`。

- `num_sets` 是不小于 4 的 2 次幂；`sampler_num` 在 `[2, num_sets/2]`。
  实际采样数量取最近的 2 次幂，平局取较小值；启动日志输出实际数量和
  `half_set_bits`。匹配集合的低 `half_set_bits` 位是唯一 sampler index。
- `sampler_assoc >= 1`，`predictor_tables` 为 1..32，`predictor_entries`
  为不小于 2 的 2 次幂，`counter_bits` 为 1..8。
- `dead_threshold` 取 0..`predictor_tables * (2^counter_bits - 1)`，使用
  `sum >= threshold`。阈值 0 表示始终预测 dead，适合边界测试。
- `partial_tag_bits` 和 `partial_pc_bits` 为 1..63，`pc_shift` 为 0..63。
  partial tag 截取 indexing policy 返回 tag 的低位；碰撞视为 sampler
  命中，属于允许的训练噪声。额外保存 secure 位，避免安全域之间的 tag 假命中。

### 哈希与训练

`pc_hash_type` 支持 `xor_fold`（默认）、`mixed`（64-bit 混合）和
`low_bits`（低位兼容模式）。先计算 `(PC >> pc_shift) ^ pc_hash_seed`，
再哈希并截取 `partial_pc_bits` 位。sampler 保存的正是该 signature。

`index_hash_type="mixed"` 对 `signature ^ table_hash_seeds[t]` 做 64-bit
混合后取索引低位。空 seed 列表按 `0x9e3779b97f4a7c15 * (t+1)` 模 2^64
派生；显式列表必须与表数等长且各值不同。不同 seed 不保证消除全部碰撞。

`index_hash_type="jwac"` 使用原始 `f1(x) + (f2(x) >> t)`，只接受不超过
32 位的 signature 和空 seed 列表。配合 `pc_hash_type="low_bits"`、
`pc_shift=0`、`pc_hash_seed=0`、`partial_pc_bits=16` 可复现单线程的
JWAC 索引。当前预测表由所有线程共享，不混入 thread/context ID，因此不能
声称与原始多线程索引完全兼容。哈希类型是经 C++ 校验的字符串参数。

计数器初值为 0。dead 训练令每张表的对应计数器饱和加一；live 训练默认
偶数表减一、奇数表右移一位。`crc_live_update=False` 时所有表统一饱和减一。
sampler 命中训练旧 signature 为 live；驱逐有效 entry 训练旧 signature 为
dead；然后保存当前 signature，并查询更新后的 predictor 得到该 entry 的
dead 标记。无效 entry 不训练，这与参考代码无条件训练旧 trace 的行为不同。

Sampler victim 顺序为无效 entry、way 顺序中的第一个 dead entry、LRU。
真实 cache victim 采用相同优先级。dead 标记只在该 entry 被访问或填充时
更新，不会随着其他 PC 的 predictor 训练而主动刷新。LRU 使用访问序号，
同一 tick 的多个访问也有确定顺序。

### 事件边界与 bypass

- `BaseSetAssoc::accessBlock()` 在 hit/miss 判定后、replacement touch 前
  通知 sampler。只采集有 PC 的 demand 请求；预取、writeback、eviction、
  无 PC 请求不训练，不把缺失 PC 当成 PC=0。
- 每次到达 tag lookup 的请求都算一次访问，包括合并进 MSHR 的请求。
  同一请求若重新经过 lookup 也会再次观察；没有额外的请求去重逻辑。
- Fill/reset 不重复训练 sampler，仅使用响应 Request 中保留的 miss PC
  查询预测表；无 PC 或被排除的类型置为 live。hit 更新 LRU 与 dead 标记。
  不带 Packet 的 reset/touch（例如块移动）使用 live 状态和 LRU 回退。
- Invalidate 清除真实 cache 的 replacement 状态，不训练、不失效独立
  sampler 中的 entry。sampler 仅通过后续访问和自身驱逐继续学习。
- `enable_bypass=True` 时，仅在 set 已满且当前填充 PC 被预测为 dead 时
  返回空 victim；有无效 cache way 时优先使用。BaseCache 使用现有临时块
  路径完成请求，不把临时块计入正常 tag store。sampler 在先前 lookup
  已经观察该请求，bypass 不会跳过或重复训练。

这是功能级实现，不模拟 predictor SRAM 端口冲突或额外访问延迟。预测器和
sampler 尚未实现 checkpoint 序列化，恢复时从冷状态重新训练。完整工作负载
性能评估、不同 coherence 流程的 bypass 回归仍需后续验证，故默认关闭 bypass。

### 验证入口

```sh
scons build/RISCV/mem/cache/replacement_policies/sdbp_core.test.opt -j8
build/RISCV/mem/cache/replacement_policies/sdbp_core.test.opt
scons build/RISCV/mem/cache/replacement_policies/sdbp_rp.test.opt -j8
build/RISCV/mem/cache/replacement_policies/sdbp_rp.test.opt
```

核心测试覆盖重叠/非重叠位段的采样数量及映射唯一性、目标数量取整、计数器
饱和和 live 更新、旧 signature 训练、无效 entry、partial-tag 碰撞、LRU、
安全域隔离、PC 高位参与哈希和非法配置。
接口测试覆盖 fill 不重复训练、响应 PC 预测、packet 类型过滤、无效/dead/LRU
优先级、无 packet 回退、满 set bypass，以及 invalidate 不影响独立 sampler。

### 香山配置的 LRU/SDBP 对照

正式性能对比使用 `configs/example/idealkmhv3.py`。可用参数：

```text
--l2-replacement-policy={default,lru,sdbp}
--l3-replacement-policy={default,lru,sdbp}
--sdbp-sampler-num=32
--sdbp-sampler-assoc=6
--sdbp-dead-threshold=8
--sdbp-pc-hash-type={xor_fold,mixed,low_bits}
--sdbp-pc-shift=1
--sdbp-enable-bypass
```

这些开关在香山配置完成后应用，避免被配置中的 DRRIP 默认值覆盖。
`default` 保留现有策略。Python `SDBPRP` 的默认 sampler 相联度仍为论文的
12；对 8-way L2 做实验时显式指定 6，并非声称 6 是最优值。其他表大小、
hash seed 等参数可通过 `SDBPRP(...)` 或 gem5 `--param` 设置。

当前 `xs-dev` 的 `idealkmhv3.py --classic-l2` 路径在平台默认配置中
使用不存在的 `slice_num` 参数，会在应用替换策略之前失败；正式对照采用
默认 sliced L2。普通 `BaseSetAssoc` 的策略接口另有单元和小型功能测试。

Slice 模式下每个 inner cache 有独立 SDBP 实例。2 MiB、8-way、64-byte
line、4 slice 对应每 slice 1024 sets，`num_sets` 从实际 cache 几何计算。
`sampler_num=32` 表示**每 slice** 32 个 sampler sets，总计 128 个；
predictor 表也每 slice 独立，不能把该配置的总存储等同于论文单个 predictor。

本地使用与性能 CI 相同的 GCPT 和固定参考模型：

```sh
export GCBV_REF_SO="$(python3 util/nemu_ref/resolve.py normal-dedup)"
python3 util/sdbp_reference/run_comparison.py \
  --output=m5out/sdbp-ideal-comparison \
  --config=configs/example/idealkmhv3.py \
  --checkpoint-root=/path/to/checkpoint \
  --checkpoint=mcf/12886 --checkpoint=libquantum/27811 \
  --sampler-assoc=6 --warmup=20000000 --measure=20000000 \
  --extra-arg=--enable-mem-dedup --jobs=6
python3 util/sdbp_reference/summarize.py m5out/sdbp-ideal-comparison
```

当前实验先聚焦替换策略，暂不继续尝试 bypass。脚本默认只跑 LRU、SDBP，
两组均不启用 bypass，固定非被测层为 LRU，所有运行使用
同一二进制、checkpoint、预取配置、warmup 和测量长度，并开启 difftest。
既有三组实验结果保留为历史记录，不改变当前两组实验的默认配置。
`--level=l3` 可改测 L3；L3 默认 mostly-exclusive，会过滤许多 PC 访问，
需检查 samplerAccesses 而不能直接假设适合训练。普通 slice 使用内嵌
restorer，不加 `--raw-cpt` 或外部 restorer 参数。

每次运行保存 `command.json`、`config.json`、`stats.txt`、`run.log`、
`status.json`；根目录的 `manifest.json` 记录二进制及参考模型 SHA256。
`tracked_changes.patch` 保存相对 HEAD 的改动，`untracked_sources.tar.gz`
补充保存 `src/`、`configs/`、`util/` 下未跟踪的源码与脚本。
汇总只取最后一个完整统计区间，检查实际测量指令数与正常退出条件。
同时检查各组 `config.json`，拒绝被测 replacement policy 以外的配置差异。
输出 `comparison.csv`、`comparison.json` 及对照表。此分支的 `demandMisses`
也可能包含 L1 预取器 requestor 转换后的请求；CSV 另外列出
`l2_cpu_data_misses`、`l2_cpu_data_mshr_misses` 和
`l2_prefetch_requestor_demand_misses`，避免把 total 误当纯 CPU load miss。
少量 point 的非加权
几何平均仅是该子集结果，不是 SPEC 总分。

LRU 基线在被测 L2 和固定的 L3 使用 `LRURP`；L1 保留香山配置原有的
策略（例如 DCache 的 TreePLRU）。这不是替换整个内存层级的策略。
原生 `LRURP` 使用 tick 更新 recency，SDBP 使用访问序号，因而同时刻
访问的次序可能不同。需要归因时可做无 dead 预测的诊断对照，不能把
实现间的所有差异直接归于 dead-block prediction。

### 计数器和 trace 的解释

统计名称前缀为 `<cache>.replacement_policy.`，重要字段包括：

| 计数器 | 含义 |
| --- | --- |
| `lookups`, `eligibleAccesses`, `noPcAccesses`, `excludedAccesses` | 访问覆盖和过滤情况 |
| `samplerAccesses`, `samplerHits`, `samplerEvictions` | 采样覆盖及独立 tag 生命周期 |
| `liveTraining`, `deadTraining` | 旧 signature 的训练次数，不包括 fill |
| `deadPredictions`, `predictionQueries` | 真实 cache 预测查询结果；bypass 查询也计入 |
| `deadVictims`, `nonLruDeadVictims`, `lruVictims` | victim 选择次数，以及真正偏离 LRU 的次数 |
| `bypasses` | 满 set 的分配尝试被拒绝次数 |
| `deadHits` | 真实 cache 中已预测 dead 的块再次被有效 demand 命中 |
| `samplerDeadHits`, `samplerDeadEvictions` | sampler 中已预测 dead 的块随后被命中或驱逐 |

`samplerDeadHits` 是 sampler 标签生命周期内观察到的误预测；
`samplerDeadEvictions` 受 sampler 自身替换策略影响，二者不能直接当作
真实 cache 的 oracle precision/recall。真实 cache 的 `deadHits` 也看不到
提前驱逐后又访问的假阳性，不能据此声称完整预测准确率。
Victim 计数在选择时增加，后续 eviction 若因 transient 状态失败，
下一次尝试可能再次计数，因此不是“成功驱逐数”。

短区间 trace 示例（tick 边界按具体 workload 选择）：

```sh
build/RISCV/gem5.opt --outdir=m5out/sdbp-trace \
  --debug-flags=SDBP --debug-start=1000000000 --debug-end=1000100000 \
  --debug-file=sdbp.trace configs/example/idealkmhv3.py \
  --generic-rv-cpt=/path/to/checkpoint.zstd \
  --l2-replacement-policy=sdbp --l3-replacement-policy=lru \
  --sdbp-sampler-assoc=6
```

Trace 输出 sampler hit/eviction、旧 signature、PC 哈希、置信度、
victim way 与原 LRU way，以及 bypass。

### 功能回归（不作为香山性能数据）

```sh
riscv64-linux-gnu-gcc -O2 -static -nostdlib -ffreestanding -fno-pie \
  -no-pie -march=rv64g -mabi=lp64d -Wl,--no-relax,-e,_start \
  util/sdbp_reference/stream_hot.c -o /tmp/sdbp-stream-hot
build/RISCV/gem5.opt --outdir=m5out/sdbp-function \
  util/sdbp_reference/se_test.py /tmp/sdbp-stream-hot --policy=sdbp
build/RISCV/gem5.opt --outdir=m5out/sdbp-bypass-function \
  util/sdbp_reference/se_test.py /tmp/sdbp-stream-hot --bypass
build/RISCV/gem5.opt --outdir=m5out/sdbp-bypass-boundary \
  util/sdbp_reference/se_test.py /tmp/sdbp-stream-hot --bypass --threshold=0
build/RISCV/gem5.opt --outdir=m5out/sdbp-no-pc \
  util/sdbp_reference/no_pc_test.py
```

`stream_hot.c` 使用 freestanding、无压缩指令的 ELF，规避此分支
TimingSimpleCPU 与面向 O3 的 RISC-V decoder 的取指边界差异。
每次运行校验数据并正常退出；这些小测试仅验证功能路径。

### 远端 Manual Performance Test

先检查目标 workflow 的输入（远端文件为准）：

```sh
gh workflow view manual-perf.yml --repo OpenXiangShan/GEM5 \
  --ref xs-dev --yaml
```

被测分支必须先存在于远端。以下两条命令示范完整 0.3 coverage 集合对照；
`--ref` 决定 workflow 版本，`branch` 决定测试代码版本：

```sh
gh workflow run manual-perf.yml --repo OpenXiangShan/GEM5 --ref xs-dev \
  -f branch=codex/sdbp -f configuration=idealkmhv3.py \
  -f benchmark_type=spec06-rva23-novec-gcc16-0.3c \
  -f note=SDBP-LRU-baseline \
  -f 'extra_args=--l2-replacement-policy=lru --l3-replacement-policy=lru'
gh workflow run manual-perf.yml --repo OpenXiangShan/GEM5 --ref xs-dev \
  -f branch=codex/sdbp -f configuration=idealkmhv3.py \
  -f benchmark_type=spec06-rva23-novec-gcc16-0.3c \
  -f note=SDBP-comparison \
  -f 'extra_args=--l2-replacement-policy=sdbp --l3-replacement-policy=lru --sdbp-sampler-assoc=6'
gh run list --repo OpenXiangShan/GEM5 --workflow manual-perf.yml \
  --event workflow_dispatch --limit 10
```

默认参考模型由 workflow 的 `util/nemu_ref/resolve.py normal-dedup`
选择，同时自动加 `--enable-mem-dedup`。旧环境中的全局 normal REF
可能不支持新版 checkpoint 的 CSR，不能把其恢复错误归因于替换策略。
本任务使用本地等价执行；上述远端命令是复现入口，不代表已经 dispatch。
