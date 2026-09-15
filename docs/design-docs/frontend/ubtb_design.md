# Kunminghu uBTB 设计说明

## 1. 定位与建模范围

uBTB 在 S1 提供快速单块预测，并通过独立的 checker 查询为 PairTAGE 的第二块提供布局。
它保留 redirect 后立即提供预测的能力：ahead 预测器尚在恢复时，uBTB 仍可按当前 PC 查询。

当前表项保存整个预测窗口内的分支布局，包含 not-taken 分支以及预测出口之后的分支。
本阶段实现布局快照；MainBTB 训练 write-through、换出 invalidate，以及 MainTAGE 双块共享
set 的协议尚未实现。因此，完整命中表示快照没有因 slot 容量被截断，不保证快照与
MainBTB 当前内容严格一致。

## 2. 参数与存储

| 参数 | 默认值 | 含义 |
|---|---|---|
| `numSets` | 64 | set 数，非零且为 2 的幂 |
| `numWays` | 4 | 每组 way 数；SMT 按线程切分时须为偶数 |
| `numSlots` | 4 | 每项最大分支 slot 数，非零 |
| `tagBits` | 22 | uBTB tag 位宽 |
| `numDelay` | 0 | 快速预测级延迟 |
| `usingS3Pred` | true | 从正常预测的 S3 BTB 布局回填 |

默认容量为 256 个块，每块至多 4 个分支；调整 set/way 不改变每块 slot 容量。
配置入口为 `src/cpu/pred/BranchPredictor.py`，运行配置可分别覆盖这些参数。

每个块项保存：

- `valid`、`startPC`、块 tag、组内 LRU 时间戳；
- 按 PC 升序排列的 `slots`，每个 slot 保存分支类型、指令长度、目标和基础方向计数器；
- `overflow`：完整输入布局的去重分支数超过 `numSlots`。

位置和目标用模拟器已有的完整地址表示。slot 向量长度限制在 `numSlots`；
overflow 时只保留有限前缀，任何预测消费者都必须先检查 `usable()`。
组内查找和替换为 O(numWays)，返回布局和方向选择为 O(numSlots)，不扫描整个 uBTB。
回填在容量有界的输入布局上排序、去重并截断。

## 3. 查询与方向选择

快速预测与 checker 共享存储，各自统计查询；checker 不覆盖主预测的 metadata。
两条读路径都区分三种状态：

| 状态 | 快速预测 | 第二块 checker |
|---|---|---|
| 完整非空布局 | 按基础方向选择 first-taken | 对各条件 slot 查询 MainTAGE，选择 first-taken |
| 完整空布局 | fall-through | 已知无分支的 fall-through |
| miss 或 overflow | fall-through 兜底 | 不可验证，不提供训练真值 |

快速预测对条件 slot 使用 `alwaysTaken || ctr >= 0`，无条件分支恒 taken；
按 PC 顺序选择第一个 taken，全部条件分支 not-taken 时走块边界。
间接跳转和 return 的基础目标来自对应 slot，后级仍可通过 ITTAGE/RAS 覆盖。

checker 将完整布局交给现有 MainTAGE 查询接口，允许覆盖每个条件 slot 的方向；
TAGE 无 provider 时使用保留下来的 slot 计数器。它仍沿用当前模型的独立 TAGE 查询，
尚不表示草案中的 H1-index/H2-tag 共享 SRAM 读已经实现。

## 4. 回填与更新

默认通过 `updateUsingS3Pred()` 保存 S3 携带的 `btbEntries`：

1. 保留当前预测窗口内所有有效 slot，包括最终出口之后的 slot；
2. 排序、按分支 PC 去重，保留 BTB 基础计数器和目标；
3. 超过 slot 容量则标记 overflow；
4. 按当前块身份更新已有项，否则在对应 set 内分配空 way 或替换 LRU。

回填不再依赖单出口置信度，也不把 S3 的 TAGE/SC 最终方向写成基础计数器。
空布局同样可以分配；从非空布局更新为空布局时不会残留旧出口。
回填重新匹配块身份，避免主预测之后的替换导致使用过期 way。

关闭 `usingS3Pred` 时保留后端更新选项：从 FTQ 的布局快照补入实际 taken 分支，
更新已执行条件 slot 的基础计数器，保留出口之后的 slot。这是兼容训练模式，
同样不提供 MainBTB 严格包含性保证。

## 5. PairTAGE 与 FTQ 适配

checker miss/overflow 时，第二块不入队，也不生成 fall-through 教师包。
P1 未改变时，PairTAGE 保留已有 P2；不可验证样本不计入第二块准确率。

完整空布局可作为独立的 fall-through 教师。第二块入队支持这种无分支表示，
不伪造 BTB 分支 slot。非空布局保留真实指令长度，包括压缩分支。

第二块 FTQ 保存已检查的全部 slot，以支持历史恢复和训练。当前仍保留从 MBTB
补充布局的模拟器兼容路径，待严格一致性协议完成后再统一数据来源。
现有入队 gating、pair override 时序和 pending-pair 训练机制不属于本阶段实现。

## 6. 统计与验证

- `predHit/predMiss`、`checkerHits/checkerMisses` 按完整布局可用性计数；
  空布局计 hit，overflow 计 miss。
- `predOverflowMisses/checkerOverflowMisses` 单独归因容量不足。
- `layoutFills/layoutOverflowFills/layoutSlots` 观察回填次数、溢出及 slot 使用量。
- `checkerHitAgreements/checkerHitDisagreements` 只统计有完整教师布局的比较。
- `twoTakenUbtbMissDrops` 包含 miss 和 overflow 导致的第二块丢弃。
- commit 分支统计按完整布局中的 slot 判断命中，条件方向正确性使用基础计数器。

针对性单测覆盖多条件 first-taken、空布局、全 not-taken、overflow 与恢复、
半对齐窗口、去重、目标/类型/指令长度、组内 LRU、ASID、SMT 和 checker metadata 隔离。

```bash
scons build/RISCV/cpu/pred/btb/test/ubtb.test.opt --unit-test --gold-linker -j64
build/RISCV/cpu/pred/btb/test/ubtb.test.opt
scons build/RISCV/gem5.opt --gold-linker -j64
```

端到端验证应使用默认 64×4×4 配置运行带 difftest 的 CoreMark，并检查布局溢出、
checker 命中率、第二块入队及历史恢复。容量变化带来的趋势需在相同配置与工作负载下比较。

2026-09-10 本地验证：16 项 uBTB 测试、5 项 FTQ 布局/历史测试通过，
修改区域的 gem5 样式检查和 `git diff --check` 通过；完整 gem5.opt 以 `-j64` 构建成功。

```bash
GCBV_REF_SO=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so \
build/RISCV/gem5.opt -d out/coremark-ubtb-block-layout-20260910 \
configs/example/kmhv3.py --raw-cpt \
--generic-rv-cpt=/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin
```

该运行启用 difftest，以 m5_exit 正常结束，执行 3,151,499 条指令。
配置确认是 64 sets、4 ways、4 slots。checker 共查询 593,086 次，
其中完整命中 508,129 次、miss 84,957 次（含 overflow 83,529 次），
第二块成功入队 216,537 次。overflow 占据大部分 checker miss，说明 slot 容量
限制已进入实际控制路径；此处只验证功能及计数，不据此推断相对旧模型的性能收益。

## 7. 实现锚点

- `src/cpu/pred/btb/btb_ubtb.hh`、`btb_ubtb.cc`：表项、查询、回填与统计
- `src/cpu/pred/btb/decoupled_bpred.cc`：第二块 checker 和 FTQ 适配
- `src/cpu/pred/btb/pairtage.cc`：教师包与不可验证时的训练处理
- `src/cpu/pred/btb/test/ubtb.test.cc`：组相联和整块布局测试
