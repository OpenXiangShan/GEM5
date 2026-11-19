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
        ("全部已观察副本驱逐且未使用的预取", pf["unused_evicted_requests"], False),
        ("右截断的已发送预取", pf["right_censored_sent_requests"], False),
    ]
    limitations = [
        "Demand 按已接受的 L1 请求计数；原始 lookup、重试和只有重试的请求另外保留。延迟从首次到达 L1 端口到 CPU 实际接收响应，包含端口反压等待。",
        "No Prediction 表示本 ROI 内、该请求到达前未见对应候选；ROI 之前的历史不作推断。候选以 PA+安全域或 VA+地址空间标识+ContextID+安全域关联，并区分发起者与缓存。",
        "Accuracy 的分母是实际下游接受的唯一请求；窗口结束仍未使用的请求属于截断，不能直接当 Unused。不同层级的流量单独输出。",
        "Coverage 使用 Prefetch-Off 的每退休指令 Miss 数作为分母，表示净 Miss 减少；可为负。它包含污染等反向效应，不能声称逐条恢复了相同动态 Miss。",
        "请求级有用、未使用驱逐、右截断互斥；只有全部已观察副本已驱逐、没有未完成发送且从未使用，才计为未使用驱逐。边界历史不完整时保守保留为截断。",
        "填充到达和 cache line ready 是不同时间点。下层填充不等于 L1 命中；权限或一致性导致的 Miss 单独列出。",
        "Pollution 仅凭一次驱逐不能确证，本报告不把驱逐或 Unused 自动归因为污染。需结合配对 Off 轨迹、受害 line 与性能证据继续验证。",
        "PC 的累计请求延迟会重叠，仅用于热点排序，不能相加为 CPU 停顿或 IPC 收益。Stride 规律来自同一地址空间及 ContextID 的实际地址序列。",
        "时间线嵌入最多 2,000 条事实，所选对象的完整事件在 timeline.csv，全部事实在 events.sqlite。"
        "用 --prefetch-id / --pc / --line 可导出指定对象及关联消费的预览。",
    ]
    if "symbolization" in summary:
        symbols = summary["symbolization"]
        limitations.append(
            f"PC 符号来自显式指定 ELF：{symbols['elf']}，SHA256 {symbols['sha256']}。"
            "缺少调试信息或不属于该 ELF 的地址保留未知；ELF 必须匹配当前工作负载。"
        )
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
    source_names = {
        int(key.removeprefix("prefetch_source_")): value
        for key, value in summary["trace"].items()
        if key.startswith("prefetch_source_")
    }
    stage_names = {
        "prediction_to_enqueue": "生成 → 首次入队",
        "enqueue_to_dequeue": "首次入队 → 发送出队",
        "dequeue_to_accept": "发送出队 → 下游接受",
        "accept_to_first_ready": "下游接受 → 首个副本就绪",
        "prediction_buffer": "预测缓冲驻留",
        "ready_queue": "发送队列驻留",
    }

    def readable(key, value):
        if key == "source":
            return (
                source_names.get(value, "Source") + f" ({value})"
                if value is not None
                else "未知来源"
            )
        if key == "accuracy_observed":
            return display(value, True)
        if key == "stage":
            return stage_names[value]
        return display(value)

    source_rows = "".join(
        "<tr>"
        + "".join(
            f"<td>{html.escape(readable(key, row[key]))}</td>"
            for key in (
                "cache",
                "source",
                "candidates",
                "aliases",
                "sent",
                "useful",
                "unused_evicted",
                "censored",
            )
        )
        + "</tr>"
        for row in summary["source_analysis"]["sources"]
    )
    stage_rows = "".join(
        "<tr>"
        + "".join(
            f"<td>{html.escape(readable(key, row[key]))}</td>"
            for key in (
                "cache",
                "source",
                "stage",
                "samples",
                "mean_ticks",
                "p90_ticks",
            )
        )
        + "</tr>"
        for row in summary["source_analysis"]["phases"]
    )
    queue_rows = "".join(
        "<tr>"
        + "".join(
            f"<td>{html.escape(readable(key, row[key]))}</td>"
            for key in (
                "actor",
                "source",
                "priority",
                "stage",
                "samples",
                "mean_ticks",
                "p90_ticks",
                "p99_ticks",
            )
        )
        + "</tr>"
        for row in summary["queue_analysis"]["stages"]
    )
    outcome_rows = "".join(
        "<tr>" + "".join(
            f"<td>{html.escape(readable(key, row[key]))}</td>"
            for key in ("actor", "source", "priority", "dequeued_requests",
                        "accepted_requests", "useful_requests", "accuracy_observed",
                        "unused_evicted_requests", "censored_requests")
        ) + "</tr>" for row in summary["queue_analysis"]["outcomes"]
    )
    delta_rows = "".join(
        "<tr>"
        + "".join(
            f"<td>{html.escape(readable(key, row[key]))}</td>"
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
    occupancy = summary["cache_occupancy"]
    occupancy_rows = "".join(
        "<tr>"
        + "".join(
            f"<td>{html.escape(display(value, percent))}</td>"
            for value, percent in (
                (row["cache"], False),
                (row["capacity_lines"], False),
                (row["mean_valid_lines"], False),
                (row["valid_lines_capacity_fraction"], True),
                (row["prefetched_lines_capacity_fraction"], True),
                (row["unused_prefetched_lines_capacity_fraction"], True),
            )
        )
        + "</tr>"
        for row in occupancy["caches"]
    )
    payload = json.dumps(
        {
            "pcs": pcs,
            "timeline": timeline,
            "failures": demand["failures"],
            "occupancy": occupancy,
            "sourceNames": source_names,
            "queues": summary["queue_analysis"]["stages"],
        },
        ensure_ascii=False,
    ).replace("<", "\\u003c")
    page = Path(__file__).with_name("report_template.html").read_text()
    page = (
        page.replace("__OCCUPANCY__", occupancy_rows)
        .replace(
            "__OCCUPANCY_LINKS__",
            (
                '<a href="cache-occupancy.csv">完整积分表</a> · '
                '<a href="cache-occupancy-timeline.csv">时间序列</a>'
                if occupancy["caches"]
                else "此 Trace 未采集占用状态。"
            ),
        )
        .replace("__QUEUE_OUTCOMES__", outcome_rows)
        .replace("__QUEUE_OUTCOME_LINKS__", (
            '完整表见 <a href="queue-outcomes.csv">逐优先级结局</a>。'
            if summary["queue_analysis"]["outcomes"] else "当前 Trace 尚无出队优先级观测。"
        ))
        .replace("__QUEUE_STAGES__", queue_rows)
        .replace(
            "__QUEUE_LINKS__",
            (
                '<a href="queue-stages.csv">分位数</a> · '
                '<a href="queue-intervals.csv">逐请求区间</a> · '
                '<a href="queue-samples.csv">原始队列采样</a>'
                if summary["queue_analysis"]["stages"]
                else "当前 Trace 尚无完整队列驻留区间。"
            ),
        )
        .replace("__PC_DELTAS__", delta_rows)
        .replace("__SOURCES__", source_rows)
        .replace("__STAGES__", stage_rows)
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
            f"| {pc['pc']} {pc.get('function') or ''} | {pc['cache']} / {pc['context_id']} | {pc['misses']} | "
            f"{main} | {pc['pattern']} | {pc['suggestion']} |"
        )
    lines += ["", "## Cache 占用", ""]
    if occupancy["caches"]:
        lines += [
            "| Cache | 容量行数 | 有效容量占比 | 预取驻留容量占比 | 未使用预取容量占比 |",
            "| --- | ---: | ---: | ---: | ---: |",
        ]
        for row in occupancy["caches"]:
            lines.append(
                f"| {row['cache']} | {row['capacity_lines']} | "
                f"{display(row['valid_lines_capacity_fraction'], True)} | "
                f"{display(row['prefetched_lines_capacity_fraction'], True)} | "
                f"{display(row['unused_prefetched_lines_capacity_fraction'], True)} |"
            )
    else:
        lines.append("此 Trace 未采集占用状态。")
    lines += ["", "## 口径与边界", ""] + ["- " + text for text in limitations]
    (Path(output) / "report.md").write_text("\n".join(lines) + "\n")
