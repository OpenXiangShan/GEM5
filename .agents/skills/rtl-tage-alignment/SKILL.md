---
name: rtl-tage-alignment
description: 对照 gem5 与 XiangShan TAGE 的 allocation/useful/reset/update 语义，分析对齐改动的性能证据。
---

# RTL TAGE 对齐

记录用户指定或实际查到的 RTL/gem5 checkout 和 commit；RTL 路径优先采用用户路径或 `XIANGSHAN_HOME`，本机 fallback 为 `/nfs/home/yanyue/workspace/xs-env/XiangShan`。不默认 pull。

## 按问题取证

- 代码语义对齐：读取 [semantic-checklist.md](references/semantic-checklist.md)，选择受改动影响的 allocation gating、victim eligibility、useful update、reset cadence 和 counter 口径。
- 性能变化归因：读取 [performance-interpretation.md](references/performance-interpretation.md)。需要从 CI 获取归档时再使用 [ci-perf-analysis](../ci-perf-analysis/SKILL.md)；已有可用产物直接复用。
- 需要动态事件对拍时使用 [tage-trace-workflow](../tage-trace-workflow/SKILL.md)。纯代码问题不要求 CI run 或 trace。

## 判断边界

- 以对应 commit 的代码和 counter 递增位置为准，区分每表 probe failure 与整次 allocation failure。
- 锁定 workload/compiler/profile、配置、窗口和 coverage 后才做严格 A/B 归因。
- 计数器变化不自动证明语义对齐或性能收益；统计提交也可能改变搜索逻辑，应检查 diff。
- 说明已确认差异、证据支持的解释及尚未排除的混杂因素；需要实验时选择能区分假设的最小 A/B。

输出包含与请求相关的版本、语义差异和证据。仅在有性能数据时报告 benchmark 变化；不为填满固定模板扩展任务。
