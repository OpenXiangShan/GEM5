# 对齐 gem5 与 RTL 的 BTB TAGE 行为

## 背景与目标

当前任务的核心，不是继续机械地“照抄 RTL 某个 patch”，而是把 gem5 中 BTB TAGE 的真实行为逐步对齐到 XiangShan RTL，并且用稳定、可信的指标判断是否真的对齐成功。

这项工作之所以复杂，是因为它同时包含：

- 行为语义对齐
- 计数器口径对齐
- SPEC06 切片性能分析
- 前端小测试和最小复现实验
- 必要的 gem5 诊断计数器补充

目前的验收目标已经从“weighted score 是否接近”收敛为：

- gem5 的 TAGE 行为能否更接近 RTL
- 关键切片上的分配与命中结构能否更接近 RTL
- 最终 `MPKI`，尤其是条件分支相关的误预测行为，能否收敛

要特别注意：最近的分析表明，RTL 某些 `commit branch type` 分类计数器本身可能不可靠。因此，后续必须区分：

- 哪些 RTL 计数器可以继续作为金标准
- 哪些 RTL 计数器只能作为辅助参考

## 当前已知信息

### 任务上下文

- 当前 gem5 分支：
  - `btb-tage-rtl-useful-sticky-align`
- RTL 源码路径：
  - `/nfs/home/yanyue/workspace/xs-env/XiangShan/src/main/scala/xiangshan/frontend/bpu/tage`
- gem5 主要代码路径：
  - [btb_tage.cc](src/cpu/pred/btb/btb_tage.cc)
  - [btb_tage.hh](src/cpu/pred/btb/btb_tage.hh)
- 结果处理配置：
  - [branch.yaml](/nfs/home/yanyue/workspace/gem5_data_proc/targets/branch.yaml)
- 当前执行计划文档：
  - [TAGE-align.md](docs/exec-plans/active/TAGE-align.md)

### 当前比较口径

当前主要使用：

- RTL：`gcc15-spec06-0.3c`
- GEM5：`gcc15-spec06-0.3c`

对应数据中，已经明确做过的 run 包括：

- RTL：
  - `cr260316-4e29a19d5-CHIConfig`
- GEM5：
  - `run446`
  - `/nfs/home/share/gem5_ci/performance_data/gcc15-spec06-0.3c/20260319_170046_1d02151_run446`

### 已确认的 RTL / GEM5 行为差异

下面这些点已经不是“猜测”，而是已有代码或数据支撑的事实：

1. `victim eligibility` 以前不对齐，gem5 原先过严，不允许替换 `strong && !useful` 项。
2. `useful update` 以前不对齐，gem5 曾经有 RTL 没有的额外 useful 清零路径。
3. `highest-table gate` 和“每个 fetch block 最多一次 allocation”曾经不对齐，后续已修正。
4. `useful` 位宽本身不是当前性能差异的主因。
5. `reset cadence` 会影响行为，但不足以单独解释当前剩余差距。

### 目前最关键的 SPEC06 结论

用 `gcc15 0.3c` 比较 RTL 和 GEM5 后，已经形成的高层结论是：

- GEM5 的总分不一定比 RTL 差，甚至整体可能略高。
- 但在很多关键 benchmark 上，GEM5 的条件分支相关误预测仍然比 RTL 更差。
- `sjeng` 是目前最干净、最稳定的负例，因此一直作为主分析切片。

### `sjeng` 上已经确认的结构性现象

`sjeng` 的核心现象已经比较稳定：

- GEM5 的 `allocateSuccess` 明显高于 RTL
- GEM5 的 `allocateFailure` 明显低于 RTL
- 但 GEM5 的条件分支误预测却更差

这已经推翻了最朴素的解释：

- 不是“GEM5 因为分配失败太多，所以 TAGE 更差”

当前更像的问题是：

- GEM5 分配得更多，也更容易成功
- 但分配到的表层级、命中结构、provider/alt 关系没有像 RTL 那样收敛成更好的状态

### 已经加入的 GEM5 诊断计数器

本轮已经加过一批 GEM5 诊断计数器，提交过的代表 commit 是：

- `1d0215103c`
- `cpu: Add BTB TAGE alignment diagnostics`

这些计数器的目标，不是改行为，而是回答下面几类问题：

- resolved / mispredict 分支最终由 provider、alt、base 谁承担
- 一个 fetch block 里是否存在多个 allocation candidate
- provider / alt 的表分布是什么
- provider 和 alt 的表间距离是什么

### `branch.yaml` 当前的使用原则

当前 `branch.yaml` 中关于 `tage` 的映射，应该严格区分：

- 已确认语义对齐、可以直接横比的项
- 只适合 GEM5 本地诊断的项

目前已经确认可直接横比的 TAGE 主线项包括：

- `resetUseful`
- `allocateSuccess`
- `allocateFailure`
- `resolveBranchHasProvider`
- `resolveBranchUseProvider`
- `resolveBranchHasAlt`
- `resolveBranchUseAltTable`
- `resolveBranchUseBaseTable`
- `mispredictBranchHasProvider`
- `mispredictBranchUseProvider`
- `mispredictBranchHasAlt`
- `mispredictBranchUseAltTable`
- `mispredictBranchUseBaseTable`
- `allocateTable_t0..t7`
- `allocateBranchProviderTable_t0..t7`

GEM5-only 的诊断项，如 prediction-time provider hit 分布，应放在独立诊断组里，不应污染共名对齐项。

### RTL commit 计数器最新判断

这是最近最重要的新发现之一。

在 RTL 小测试上已经确认：

- `commit_branch_num` 看起来基本可信
- `commit_branch_mispredicts` 看起来也大概率可信
- 但 `commit_branch_type_conditional` 明显解释不通，不能再当作真实 committed conditional branch 数量

以 `tage1` 为例：

- 源码和反汇编都支持函数主体应有大约 `11000` 条 conditional branches
- RTL 实跑却给出 `commit_branch_type_conditional = 6516`
- 同类问题在 `alternating_test`、`two_bit_pattern_test` 上也重复出现

因此，当前要明确：

- `commit_branch_type_conditional` 不可靠
- `BpBInsts` 不能继续用这条 RTL 计数器来充当

但与此同时，另一个结论也成立：

- RTL 的 `cond_MPKI` 在 SPEC06 0.3c CSV 中，逐行都严格等于 `BpBWrong * 1000 / 20M`

这意味着：

- `cond_MPKI` 的计算链路本身没有乱
- 真正悬而未决的是：`commit_branch_mispredicts_type_conditional` 本身是否也存在语义问题

当前判断是：

- `total_MPKI` 比 `cond_MPKI` 更稳
- `cond_MPKI` 还能参考，但可信度要低于过去的假设

## 假设与待验证问题

### 假设 1：真正的主差异在 hit-set / history folding / tag-index 形成方式

这是当前最强的怀疑方向。

支持它的现象包括：

- GEM5 的 `resolveBranchHasAlt` 在多个 benchmark 上显著高于 RTL
- provider / alt 往往来自相邻表
- GEM5 虽然往高表分配，但高表没有像 RTL 那样稳定承担 provider

还需要继续验证：

- PHR / folded history 是否和 RTL 真正一致
- prediction-time 和 update-time 重算使用的 hit set 是否一致
- index / tag 相关性是否让相邻表过度同时命中

### 假设 2：base predictor / mainBTB 交互仍然存在差异

这条目前不是第一嫌疑，但仍然没有排除。

原因是：

- 当 provider 不存在或不够强时，最终预测仍会落回 `alt/base`
- 如果 mainBTB 的行为和 RTL 不一致，会间接影响：
  - 最终预测准确率
  - useful 的增长条件
  - `useAltOnNa` 的训练输入

### 假设 3：RTL 的 conditional 分类 mispredict 计数也可能存在口径问题

目前已经确认的是：

- `commit_branch_type_conditional` 不可靠

尚未确认的是：

- `commit_branch_mispredicts_type_conditional`
- `commit_conditional_branch_mispredicts_reason_TAGE`
- `commit_conditional_branch_mispredicts_reason_BTB`

这些 conditional 子类 mispredict 计数到底是否可信，还需要继续验证。

这件事很重要，因为如果这条路也不可靠，那么后续对 `cond_MPKI` 的使用方式就必须调整。

### 假设 4：bank conflict 只能解释部分差异，不是主因

当前更倾向于：

- RTL 的 bank conflict 确实会压低 `hasAlt`
- 但这条线更像“部分解释”
- 不太像能单独解释 `sjeng` 这类 benchmark 上明显的误预测差距

### 已经相对排除的方向

下面这些方向已经不再是当前第一优先级：

1. `useful` 位宽是主因
2. 一个 fetch block 内多个 allocation candidate 被遍历顺序压掉
3. RTL `notNeedUpdate` suppression 是当前准确率主因
4. “allocation failure 太高导致预测差”这个朴素解释
5. `provider/alt` 选取规则本身和 RTL 完全不同

这里最后一条要表述准确：

- 当前更像是命中集合不一样
- 不是从同一批命中里挑 provider / alt 的规则不一样

## 计划步骤

1. 重新整理 RTL 计数器可信度分层。
   目标：明确哪些 RTL 计数器可以继续作为验收依据，哪些只能当辅助参考。
   产出：更新 `branch.yaml` 的使用口径，必要时删除或标注不可靠映射。

2. 优先核对 RTL 中 `commit_branch_num`、`commit_branch_mispredicts`、`commit_branch_type_*`、`commit_branch_mispredicts_type_*` 的定义位置。
   目标：确认总量计数和分类计数分别挂在哪一级、统计条件是什么。
   产出：一份“RTL branch 相关 perf 可信度说明”。

3. 继续以 `sjeng` 为主切片，结合 `gcc`、`gobmk`、`omnetpp` 做结构对比。
   目标：继续确认 GEM5 分配更多却预测更差，到底对应哪些结构差异。
   产出：分 benchmark 的 provider/alt/base / allocation 结构对照。

4. 回到 PHR / folded history / tag-index 形成方式，逐段对照 RTL 和 gem5。
   目标：验证当前最强怀疑是否成立。
   产出：代码级差异列表，必要时做最小行为 patch。

5. 如果代码对照仍不足以解释差异，进入 trace 阶段。
   目标：对单个小测试或单个 SPEC 切片做 RTL / GEM5 的逐分支对比。
   产出：可复现的最小 trace 差异点。

6. 在确认统计口径可靠后，再决定后续验收指标。
   目标：明确到底继续用 `cond_MPKI`，还是暂时转向 `total_MPKI + TAGE 内部计数器` 组合。
   产出：后续分析统一口径。

## 验证方式

这项任务的“成功”不是一次提交，而是一组标准逐步满足。

当前阶段的验证标准如下：

### 分析阶段完成标准

- 已明确 RTL branch 相关计数器的可信度分层
- 已确认当前最可能的差异源头，而不是只停留在模糊猜测
- 已排除若干错误方向
- 已形成下一阶段最有价值的实验方案

### 行为对齐阶段完成标准

- GEM5 与 RTL 在关键 benchmark 上的 TAGE 主线计数器更接近
- `allocateTable_*` 和 `allocateBranchProviderTable_*` 分布明显收敛
- provider / alt 结构差异缩小

### 性能验收阶段完成标准

- 在确认计数器口径可靠后，GEM5 的目标 MPKI 指标更接近 RTL
- 或者至少已经明确：剩余差距来自哪些尚未对齐的路径

## 进度

- [x] 2026-03-18 梳理早期 useful / allocation / gating 对齐提交，确认 useful 位宽不是主问题。
- [x] 2026-03-19 把 `gcc15-spec06-0.3c` 作为当前 RTL / GEM5 主比较口径。
- [x] 2026-03-19 在 GEM5 中加入 TAGE 对齐诊断计数器，并完成对应提交与 CI。
- [x] 2026-03-19 用 `sjeng` 作为主切片，确认“分配更多但预测更差”不是 allocation failure 过高导致。
- [x] 2026-03-19 确认单 fetch block 多 allocation candidate 不是当前主因。
- [x] 2026-03-19 验证 `notNeedUpdate` suppression 不是当前准确率主因。
- [x] 2026-03-19 完成 provider / alt / per-table 分布初步诊断。
- [x] 2026-03-24 通过 `tage1`、`alternating_test`、`two_bit_pattern_test` 确认 RTL `commit_branch_type_conditional` 解释不通。
- [x] 2026-03-24 确认 RTL `commit_branch_num` 与 `commit_branch_mispredicts` 看起来比 `commit_branch_type_*` 更可信。
- [ ] 继续定位 RTL `commit_branch_mispredicts_type_conditional` 的定义与可信度。
- [ ] 决定后续主要验收指标是否暂时从 `cond_MPKI` 部分转向 `total_MPKI`。
- [ ] 回到 PHR / folded history / tag-index 代码路径，继续做 RTL vs gem5 对照。
- [ ] 如有必要，进入小测试或 SPEC 切片 trace 对比。

## 发现与意外情况

- 原先默认认为 RTL 的 `commit_branch_type_conditional` 可以作为 `BpBInsts`，这个前提现在已经被推翻。
- `SPEC06` 中的 `cond_MPKI` 计算链路本身是自洽的，但它依赖的 conditional mispredict 子类计数器仍需继续审视。
- 小测试说明 branch type 分类计数器有问题，但还没有直接证明总 mispredict 计数也坏了。
- `sjeng` 的现象非常稳定，仍然是最有价值的主切片。
- 当前最像的问题不是“分配太少”或“分配失败太多”，而是“分配结构和命中结构没有对齐”。

## 决策记录

- 决策：不再把“某个 RTL patch 的字面语义”当作唯一对齐目标。
  - 原因：照抄局部逻辑并不能自动复现 RTL 的整体行为。
  - 日期：2026-03-19

- 决策：以 `sjeng` 为主切片做本地深入分析。
  - 原因：它最稳定地体现了“分配更多但预测更差”的反常现象。
  - 日期：2026-03-19

- 决策：把 `branch.yaml` 中可共名对齐的 TAGE 计数器收敛为严格语义一致集合。
  - 原因：避免计数器名字相同但含义不同，污染后续分析。
  - 日期：2026-03-19

- 决策：不再把 RTL `commit_branch_type_conditional` 当作 `BpBInsts` 使用。
  - 原因：多个前端小测试已经证明这个计数器不能解释源码和反汇编对应的实际条件分支数量。
  - 日期：2026-03-24

- 决策：在确认 conditional 分类 mispredict 计数器可靠前，`total_MPKI` 的参考优先级高于 `cond_MPKI`。
  - 原因：当前总量计数看起来比 type 分类计数更稳定。
  - 日期：2026-03-24

## 交接提示

如果新开会话继续推进，建议优先做下面两件事：

1. 先去 RTL 代码里找 `commit_branch_num`、`commit_branch_mispredicts`、`commit_branch_type_*`、`commit_branch_mispredicts_type_*` 的 perf 定义位置，把计数口径彻底钉死。
2. 在此基础上，再决定后续主验收指标是否继续使用 `cond_MPKI`，还是先切到 `total_MPKI + TAGE 内部结构计数器`。

如果还要继续做行为对齐，不建议回到“逐个 reset/useful 小细节拍脑袋试”的方式。当前最值钱的主线，仍然是：

- `sjeng` 结构差异
- hit-set / PHR / folded history / tag-index
- provider / alt 命中结构
