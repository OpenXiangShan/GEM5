# PHAST MDP 交付说明

本文档用于交接 XS-GEM5 中的 PHAST memory dependence predictor 实现。
目标是让接手者在不依赖上下文聊天记录的情况下，快速知道：

1. PHAST 学的是什么
2. 代码落在哪些文件
3. 怎么开关和调参
4. 新增 stats 应该怎么看
5. 目前已知的风险和后续工作点

本文档对应的代码状态以 `2026-07-31` 为准。

## 1. PHAST 解决的问题

XS 里原本的 MDP 是 StoreSets：

`Load PC -> StoreSet -> 当前最新 Store`

它能减少重复的 store-load violation，但粒度比较粗，容易把不同路径上的 load 合并到同一组里，形成假依赖。

PHAST 的思路是：

`Load PC + path history -> store distance -> 当前 Store Queue 中的具体动态 Store`

也就是说，PHAST 不再预测“属于哪个 StoreSet”，而是直接预测“这个 load 应该等当前 SQ 中第几个更老的 store”。

## 2. 当前实现状态

PHAST 已经接入 XS-GEM5 的 O3 内核，不再是纯实验代码。

已接入的主路径：

- `src/cpu/o3/phast.hh`
- `src/cpu/o3/phast.cc`
- `src/cpu/o3/mem_dep_unit.hh`
- `src/cpu/o3/mem_dep_unit.cc`
- `src/cpu/o3/lsq_unit.cc`
- `src/cpu/o3/iew.cc`
- `src/cpu/o3/commit.cc`
- `src/cpu/o3/dyn_inst.hh`
- `src/cpu/o3/BaseO3CPU.py`
- `configs/common/xiangshan.py`
- `configs/example/kmhv3.py`

当前行为：

- `EnablePHASTMDP=true` 是当前默认
- `--no-enable-phast-mdp` 可以显式关闭 PHAST
- PHAST 预测结果会先映射成当前 SQ 中的动态 store，再走现有 replay-based MDP 路径
- RAW violation 现在会在 IEW 违例点立即训练 PHAST，commit 阶段还有一次兜底训练，但已用 `violationTrained` 去重

## 3. 算法语义

### 3.1 查表 key

PHAST 查表使用：

- `Load PC`
- `path history`

其中 `path history` 取的是 load 之前的分歧分支历史。当前实现里会先按 `seqNum < load_seq_num` 过滤，再按历史长度并行查多张表。

当前历史长度表是：

`0, 2, 4, 6, 8, 12, 16, 32`

### 3.2 表项含义

表项里保存的是：

- `store_distance`
- `confidence counter`
- `tag`

`store_distance` 是一个相对距离，不是 store seqNum，也不是 store PC。

含义示例：

```text
S5  distance = 0
S4  distance = 1
S3  distance = 2
S2  distance = 3
```

如果 load 真正依赖 `S3`，预测值应该是 `store_distance = 2`。

### 3.3 从 distance 到动态 store

PHAST 命中后，不会直接拿 distance 去喂 LSQ，而是先在当前 store queue 里把它换成具体的动态 store seqNum。

换算方式本质是：

`load.sqIt - (distance + 1)`

如果这个位置已经不在当前 SQ 范围内，或者 iterator 无效，预测会被丢弃。

### 3.4 训练

PHAST 仍然是 violation-driven training。

当真实 RAW violation 发生时，会用：

- load PC
- filtered branch history
- violating store 的 SQ distance

来更新对应表项。

### 3.5 commit-time confidence update

load 如果确实拿到了有效 PHAST 预测，并且后来 commit 时可以验证该预测对应的 store 地址与 load 地址是否冲突，就会更新 PHAST 表的 confidence。

这个更新的作用是压 alias 和错误预测，不是重新学习 distance。

## 4. 代码地图

### 4.1 核心 predictor

`src/cpu/o3/phast.hh/.cc`

负责：

- 多历史长度表
- hash/index/tag
- confidence counter
- 路径历史 hash
- distance 预测和训练

### 4.2 依赖单位集成

`src/cpu/o3/mem_dep_unit.cc`

负责：

- 选择 StoreSets 或 PHAST
- PHAST distance -> current SQ store 映射
- replay-based MDP 依赖表填写
- PHAST 违例训练
- commit-time confidence update
- 新增 stats

### 4.3 LSQ replay

`src/cpu/o3/lsq_unit.cc`

负责：

- 用 `mdpProducingStores` 挡住 load replay
- 在 load commit 时把预测信息交给 `InstQueue::commit()`

### 4.4 违例路径

`src/cpu/o3/iew.cc`

负责：

- 发现 RAW violation
- 把 violation 交给 `MemDepUnit::violation()`

`src/cpu/o3/commit.cc`

负责：

- squash 后的 load 退役兜底训练
- 用 `violationTrained` 避免重复训练

### 4.5 per-load 元数据

`src/cpu/o3/dyn_inst.hh`

`MemDepInfo` 里现在保存：

- `predicted`
- `predStoreAddrs`
- `predStoreSizes`
- `predBranchHistLength`
- `predictorHash`
- `violatingStoreSeqNum`
- `violatingStorePC`
- `storeQueueDistance`
- `violationCounted`
- `violationTrained`

## 5. 参数和默认值

### 5.1 配置入口

PHAST 参数定义在 `src/cpu/o3/BaseO3CPU.py`。配置脚本可以直接给 CPU
SimObject 赋值；当前 `kmhv3.py` 固定关闭 PHAST，`idealkmhv3.py` 固定开启
PHAST，均使用 `BaseO3CPU` 中的表参数默认值。

### 5.2 默认值

默认值定义在 `src/cpu/o3/BaseO3CPU.py`：

| 参数 | 默认值 | 含义 |
|---|---:|---|
| `EnablePHASTMDP` | `True` | 默认开启 PHAST |
| `phast_num_rows` | `64` | 每张历史表的行数 |
| `phast_associativity` | `4` | 组相联路数 |
| `phast_tag_bits` | `16` | tag 位宽 |
| `phast_max_counter` | `16` | confidence counter 上限 |
| `phast_counter_threshold` | `1` | 发出 PHAST 预测所需的最小 confidence |
| `phast_counter_increment` | `0` | 正确预测后的置信度增量；`0` 保持原有的直接恢复上限语义 |
| `phast_counter_decrement` | `1` | 错误预测后的置信度减量 |
| `phast_selected_target_bits` | `5` | 参与 path hash 的 target 地址低位数 |
| `phast_history_lengths` | `[0, 2, 4, 6, 8, 12, 16, 32]` | 每张 path table 对应的 branch history 长度，必须从 `0` 开始且严格递增 |
| `phast_second_target_max_distance` | `0` | 第二个 store distance 的排他上限；`0` 表示虚拟 SQ 容量的一半 |

### 5.3 当前需要注意的脚本

如果其它入口脚本覆盖了 PHAST 参数，要确认其值被赋给每个 CPU 的对应
SimObject 属性；否则会继续使用 `BaseO3CPU.py` 的默认值。

## 6. 计数器口径

### 6.1 PHAST 相关

| 计数器 | 含义 |
|---|---|
| `phastTableHits` | PHAST 查表命中，且返回了非零 / 有效 `store_distance` |
| `phastEffectivePreds` | 命中的 `store_distance` 成功映射到当前 SQ 中的动态 store，并真的给 load 建了依赖 |
| `phastDropInvalidSQDistance` | 命中后因为 `store_distance` 非法、SQ 游标无效或目标 store 找不到，预测被丢弃 |
| `phastViolationUpdates` | PHAST 违例训练次数 |
| `phastCommitUpdates` | PHAST commit-time confidence update 次数 |

### 6.2 通用 MDP 违例口径

| 计数器 | 含义 |
|---|---|
| `mdpUnpredictedViolations` | load 没被 MDP 预测，但后来发生真实 RAW violation |
| `mdpPredictedViolations` | load 已经有 MDP 预测，但后来仍发生真实 RAW violation |
| `mdpFalseDepAtCommit` | load 曾被预测依赖某个 store，但 commit 时确认预测 store 地址与 load 地址不冲突 |

### 6.3 现有计数器的关系

现在 `phastPredictions` 还保留着，主要是兼容旧分析脚本。

它和 `phastEffectivePreds` 目前表达的是同一个成功映射事件，接手时不要把它们当成两类独立语义。

如果做新分析，建议优先看：

1. `phastTableHits`
2. `phastEffectivePreds`
3. `phastDropInvalidSQDistance`
4. `mdpUnpredictedViolations`
5. `mdpPredictedViolations`
6. `mdpFalseDepAtCommit`

## 7. 怎么看结果

### 7.1 读法建议

如果 `phastTableHits` 很低：

- 说明表项根本没学到，或者 path hash / history length 不对

如果 `phastTableHits` 高，但 `phastEffectivePreds` 低：

- 说明表是命中的，但 distance 没法映射成当前 SQ 里的动态 store

如果 `phastEffectivePreds` 高，但 `phastViolationUpdates` 仍然很低：

- 说明预测虽然落地了，但 violation 反馈没打通，或者 workload 本身很少真的触发 RAW violation

如果 `mdpUnpredictedViolations` 很高：

- 说明漏预测多，false negative 多

如果 `mdpPredictedViolations` 很高，同时 `mdpFalseDepAtCommit` 也高：

- 说明预测到了一些 store，但依赖对象不准，或过度保守导致假依赖

### 7.2 SPEC 的统计口径

SPEC CPU 06 的输出目录里通常是多个 slice，每个 slice 下面都有自己的 `stats.txt`。

比较 PHAST 时，建议先看单个代表 slice 的趋势，再决定是否做整 benchmark 汇总。

如果要做整 benchmark 级比较，优先按 slice 聚合后再做加权，而不是直接把不同 slice 的原始计数当成同一个事件流。

## 8. 验证方式

### 8.1 编译

```bash
scons build/RISCV/gem5.opt --gold-linker -j64
```

或者至少编 touched 的 O3 对象：

```bash
scons build/RISCV/cpu/o3/dyn_inst.o \
      build/RISCV/cpu/o3/mem_dep_unit.o \
      build/RISCV/cpu/o3/inst_queue.o \
      build/RISCV/cpu/o3/iew.o \
      build/RISCV/cpu/o3/commit.o \
      build/RISCV/cpu/o3/lsq_unit.o -j64
```

### 8.2 运行

```bash
./build/RISCV/gem5.fast ./configs/example/kmhv3.py \
  --generic-rv-cpt=<checkpoint>
```

### 8.3 建议先看的 slice

```text
spec_all/perlbench_checkspam_24678
```

这个 slice 是对store-load违例较多次数的slice.

开发时曾发现一个问题：PHAST 已经打开，但 `phastViolationUpdates` 一直是 0。根因是 IEW 的 RAW violation 路径曾经把 PHAST 训练挡掉了；当前代码已经改成在 violation 点直接训练，并用 `violationTrained` 去重。

## 9. 已知风险

1. 现在 `phastPredictions` 和 `phastEffectivePreds` 语义有重叠，后续如果要做长期维护，建议保留一个主口径，另一个做兼容或删掉。
2. `mdpFalseDepAtCommit` 依赖 load commit 时已经记录过的预测 store 地址；如果没记录到地址，它不会被算成 false positive。
3. PHAST 的效果强依赖 branch history 质量和 table 参数，特别是 `phast_num_rows` 和 `phast_tag_bits`。
4. 如果改用别的 config 入口，要检查 PHAST 参数是否也被传到了 CPU。
5. 最近一次代码变更后，建议重新跑一次代表 slice，确认新增的 `phast*` 和 `mdp*` 计数器都按预期增长。

## 10. 给接手者的建议

1. 先用 `perlbench_checkspam_24678` 做最小回归。
2. 看 `phastTableHits -> phastEffectivePreds -> phastViolationUpdates` 这条链路是否闭环。
3. 如果 `phastDropInvalidSQDistance` 偏高，优先查 SQ iterator / distance 映射，而不是先怀疑 hash。
4. 如果 `mdpFalseDepAtCommit` 偏高，再看 commit-time overlap 判定和 `LSQDepCheckShift`。
5. 如果要做参数搜索，优先扫 `phast_num_rows`、`phast_tag_bits`，其次才是 `phast_associativity` 和 `phast_max_counter`。
