#!/usr/bin/env python3
"""Self-contained offline report with searchable PCs and a fact timeline."""

import html
import json
from pathlib import Path


def display(value, percent=False):
    if value is None:
        return "未测 / 不适用"
    if isinstance(value, float):
        return f"{value * 100:.2f}%" if percent else f"{value:,.3f}"
    return f"{value:,}" if isinstance(value, int) else str(value)


def write_report(output, summary, pcs, timeline, runs, pc_deltas):
    demand, pf = summary["demand"], summary["prefetch"]
    paired = summary.get("versus_prefetch_off", {})
    metrics = [
        ("L1 Demand 请求", demand["requests"], False),
        ("Residual Miss", demand["misses"], False),
        ("预取候选", pf["candidates"], False),
        ("下游接受的唯一预取", pf["sent_requests"], False),
        ("已观察到的 Accuracy", pf["accuracy_observed"], True),
        ("有用预取的 Timeliness", pf["timeliness"], True),
        ("对 Prefetch-Off 的净 Miss Coverage", paired.get("net_miss_coverage"), True),
        ("对 Prefetch-Off 的 IPC 变化", paired.get("ipc_speedup"), True),
        (
            "对原始 Prefetch-On 的 IPC 变化",
            summary.get("versus_original", {}).get("ipc_speedup"),
            True,
        ),
        ("发送预取请求字节（去重）", pf["prefetch_request_bytes"], False),
        ("Demand 平均延迟 / tick", demand["mean_latency_ticks"], False),
        ("未完成 Demand", demand["right_censored"], False),
        ("命中但预取晚于端口到达", demand["late_prefetch_hits"], False),
        ("未使用且已驱逐的预取", pf["unused_evicted_requests"], False),
        ("右截断的已发送预取", pf["right_censored_sent_requests"], False),
    ]
    limitations = [
        "Demand 按已接受的 L1 请求计数；原始 lookup、重试和只有重试的请求另外保留。延迟从首次到达 L1 端口到 CPU 实际接收响应，包含端口反压等待。",
        "No Prediction 表示本 ROI 内、该请求到达前未见对应候选；ROI 之前的历史不作推断。候选以 PA+安全域或 VA+地址空间标识+ContextID+安全域关联，并区分发起者与缓存。",
        "Accuracy 的分母是实际下游接受的唯一请求；窗口结束仍未使用的请求属于截断，不能直接当 Unused。不同层级的流量单独输出。",
        "Coverage 使用 Prefetch-Off 的每退休指令 Miss 数作为分母，表示净 Miss 减少；可为负。它包含污染等反向效应，不能声称逐条恢复了相同动态 Miss。",
        "填充到达和 cache line ready 是不同时间点。下层填充不等于 L1 命中；权限或一致性导致的 Miss 单独列出。",
        "Pollution 仅凭一次驱逐不能确证，本报告不把驱逐或 Unused 自动归因为污染。需结合配对 Off 轨迹、受害 line 与性能证据继续验证。",
        "PC 的累计请求延迟会重叠，仅用于热点排序，不能相加为 CPU 停顿或 IPC 收益。Stride 规律来自同一地址空间及 ContextID 的实际地址序列。",
        "时间线嵌入最多 2,000 条事实，所选对象的完整事件在 timeline.csv，全部事实在 events.sqlite；用 --pc / --line 可生成指定对象的预览。",
    ]
    audit = summary.get("counter_reconciliation", {})
    limitations.append(
        f"原生 Counter 核对：{audit.get('status', 'not_checked')}，{len(audit.get('checks', []))} 项；详见 audit.json。"
    )
    if audit.get("status") == "mismatch":
        limitations.append(
            "当前 Trace 与原生 Counter 存在不一致；修复或解释差异后才能用于性能结论。"
        )
    if pf["right_censored_sent_requests"]:
        limitations.append("存在右截断生命周期，当前 Accuracy 是本观测窗口内的已确认比例。")
    if paired.get("reason"):
        limitations.append(paired["reason"])
    for key, label in (
        ("versus_prefetch_off", "Prefetch-Off"),
        ("versus_original", "原始 Prefetch-On"),
    ):
        if summary.get(key, {}).get("useful_without_ipc_gain"):
            limitations.append(
                f"相对{label}观察到及时预取使用，但 IPC 未提高；请结合 TEA/DIP 检查关键路径与其他瓶颈。"
            )
    comparison = []
    for label, run in runs.items():
        stats = run["stats"] or {}
        comparison.append(
            {
                "variant": label,
                "cycles": stats.get("cycles"),
                "retired": stats.get("retired_instructions"),
                "ipc": stats.get("ipc"),
                "misses": run["demand"]["misses"],
                "latency": run["demand"]["mean_latency_ticks"],
                "pf_bytes": run["prefetch"]["prefetch_request_bytes"],
            }
        )
    cards = "".join(
        f'<div class="card"><span>{html.escape(label)}</span><b>{display(value, percent)}</b></div>'
        for label, value, percent in metrics
    )
    table = "".join(
        "<tr>"
        + "".join(
            f"<td>{html.escape(display(row[key]))}</td>"
            for key in ("variant", "cycles", "retired", "ipc", "misses", "latency", "pf_bytes")
        )
        + "</tr>"
        for row in comparison
    )
    notes = "".join("<li>" + html.escape(text) + "</li>" for text in limitations)
    delta_rows = "".join(
        "<tr>"
        + "".join(
            f"<td>{html.escape(display(row[key]))}</td>"
            for key in (
                "pc",
                "cache",
                "address_space",
                "misses_before",
                "misses_after",
                "weighted_wait_delta_ticks_per_inst",
                "pending_before",
                "pending_after",
            )
        )
        + "</tr>"
        for row in pc_deltas[:30]
    )
    payload = json.dumps(
        {"pcs": pcs, "timeline": timeline, "failures": demand["failures"]}, ensure_ascii=False
    ).replace("<", "\\u003c")
    page = Path(__file__).with_name("report_template.html").read_text()
    page = (
        page.replace("__PC_DELTAS__", delta_rows)
        .replace("__CARDS__", cards)
        .replace("__COMPARISON__", table)
        .replace("__NOTES__", notes)
        .replace("__DATA__", payload)
    )
    (Path(output) / "report.html").write_text(page)
    lines = [
        "# GEM5 预取失效分析",
        "",
        f"输入：`{summary['run']}`",
        "",
        "| 指标 | 值 |",
        "| --- | ---: |",
    ]
    lines += [f"| {label} | {display(value, percent)} |" for label, value, percent in metrics]
    lines += ["", "## Residual Miss", "", "| 类别 | 次数 |", "| --- | ---: |"]
    lines += [f"| {key} | {value} |" for key, value in demand["failures"].items()]
    lines += [
        "",
        "## Hot PCs",
        "",
        "| PC | Cache / Context | Miss | 主要原因 | 模式 | 建议 |",
        "| --- | --- | ---: | --- | --- | --- |",
    ]
    for pc in pcs[:30]:
        main = max(pc["failures"], key=pc["failures"].get, default="")
        lines.append(
            f"| {pc['pc']} | {pc['cache']} / {pc['context_id']} | {pc['misses']} | "
            f"{main} | {pc['pattern']} | {pc['suggestion']} |"
        )
    lines += ["", "## 口径与边界", ""] + ["- " + text for text in limitations]
    (Path(output) / "report.md").write_text("\n".join(lines) + "\n")
