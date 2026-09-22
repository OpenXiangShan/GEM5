---
name: ci-perf-pr-report
description: 为 GEM5 PR 准备或发布性能对比报告，选择可比 baseline 并汇总分数与关键计数器证据。
---

# CI 性能 PR 报告

把已完成的性能 CI 转成可审阅、可追溯的 PR 评论。复用相邻的
`ci-perf-analysis` skill 取数；不包含 RTL 对齐或特定 predictor 分析。

## 输入

尽量取得：

- candidate run URL/ID；
- baseline run URL/ID，或足以定位 baseline 的 commit/config/profile；
- 目标 PR；
- feature 说明，以及是否明确授权发布评论。

缺少 baseline 时可以搜索，但不得静默选择口径不一致的 run。

## 工作流

1. 复用已有可追溯的归档和处理结果；需要定位或处理 CI 数据时读取 `../ci-perf-analysis/SKILL.md`。
2. 检查 candidate 和 baseline 的 `status`、`conclusion`、SHA、workflow、配置、
   benchmark/profile、coverage、extra args 和 abort 数。run 未完成时说明状态和缺口；
   用户要求等待完成或持续跟进时才继续监控，不把“已创建”写成“已通过”。
3. baseline 优先级：用户明确指定 > 同 SHA 的 feature-off control > 同配置的相邻
   main/weekly baseline。若编译器、checkpoint、coverage 或关键参数不同，明确标注
   confounder，不做严格因果归因。
4. 从原始 `score.txt` 和处理后的 CSV 交叉核对 Overall、SPECint、SPECfp；展开
   benchmark 子项的 raw delta 和相对变化，不只报告总分。
5. 只选择能解释 feature 的少量 stats：
   - 通用：IPC/CPI、Topdown frontend/bad-spec/backend/memory；
   - 前端：fetch/recovery bubble、fragment stall、branch miss；
   - 后端/访存：dispatch stall、core/memory bound、cache MPKI/latency。
   优先比较 rate、MPKI 或 weighted 指标；指令数不同时不要直接解释 raw count。
6. 用重点 benchmark 和计数器证据解释收益/回退，区分确认事实、合理推断和未决问题。
7. 生成简洁的英文 PR 评论；已有明确发布授权时直接发布并回查评论 URL，否则交付 draft。

## PR 评论结构

```markdown
## Performance result

- Candidate: <run, SHA, config/profile>
- Baseline: <run, SHA, config/profile>
- Completeness: <coverage, aborts>

### Score
| Suite | Baseline | Candidate | Change |

### Benchmark breakdown
<主要收益、回退及完整 suite 子项>

### Counter evidence
<少量与 feature 直接相关的 weighted stats>

### Interpretation
<结论、因果边界和剩余风险>
```

PR 评论中直接链接 Actions run；不要包含 Codex memory citation。若数据不完整，只准备状态更新；已有发布授权时才发布，不给最终性能结论。除非用户明确要求更新既有评论，否则发布一条新评论。

## 完成标准

- 两组数据的比较口径已审计；
- score 与 benchmark 子项已交叉核对；
- 有可比 stats 时，至少一组相关计数器支持或限制结论；否则明确数据缺口；
- 已授权时评论发布成功并返回 URL，否则提供可直接发布的 draft。
