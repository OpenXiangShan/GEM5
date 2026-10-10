#!/usr/bin/env python3
"""Bounded, evidence-first candidate analysis of completed PerfCCT slices."""

import argparse
from collections import Counter
import csv
import json
from pathlib import Path
from perfcct_diagnose import WAIT_WAKE_SOURCES, final_stats


def read_json(path, default=None):
    return json.loads(path.read_text()) if path.exists() else default


def option(command, name):
    for index, token in enumerate(command):
        if token.startswith(name + "="):
            return token.split("=", 1)[1]
        if token == name and index + 1 < len(command):
            return command[index + 1]
    return None


def execution_gate(validation, off_command, on_command, diagnosis):
    gaps = []
    if validation.get("status") != "passed":
        gaps.append("overall slice validation has not passed")
    for side in ("off", "on"):
        run = validation.get(side, {})
        for key, value in (
            ("status", "passed"),
            ("host_exit_status", 0),
            ("difftest_enabled", True),
            ("reference_initialized", True),
            ("stats_sections", 2),
        ):
            if run.get(key) != value:
                gaps.append(f"{side}.{key} is missing or not {value}")
        if run.get("errors") or run.get("fatal_or_difftest_errors"):
            gaps.append(f"{side} reports completion errors")
        if not run.get("maxinst_exit_ticks"):
            gaps.append(f"{side} has no maxinst exit marker")
    on = validation.get("on", {})
    if str(on.get("trace_end_tick")) not in [
        str(t) for t in on.get("maxinst_exit_ticks", [])
    ]:
        gaps.append("causal trace end does not match the maxinst exit")
    timing = validation.get("timing_invariance", {})
    differences = timing.get("difference_counts")
    if differences is None:
        differences = [
            s.get("difference_count") for s in timing.get("segments", [])
        ]
    if timing.get("status") != "passed" or differences != [0, 0]:
        gaps.append(
            "two complete OFF/ON non-host statistics comparisons have not passed"
        )
    for side, command in (("off", off_command), ("on", on_command)):
        argv = command.get("command", command.get("argv", []))
        if (
            option(argv, "--warmup-insts-no-switch") != "5000000"
            or option(argv, "--maxinsts") != "10000000"
        ):
            gaps.append(
                f"{side} is not the required 5M warmup + 5M measurement"
            )
    for key in ("binary_sha256", "reference_sha256"):
        if not off_command.get(key) or off_command.get(key) != on_command.get(
            key
        ):
            gaps.append(f"matching {key} is unavailable")
    off_argv = off_command.get("command", off_command.get("argv", []))
    if (
        "--arch-db-dump-causal" in off_argv
        or "--arch-db-dump-lifetime" in off_argv
    ):
        gaps.append("OFF baseline has instruction tracing enabled")
    if diagnosis.get("roi_outside_known_trace") is not False:
        gaps.append(
            "measurement ROI is outside the trace or its coverage is unknown"
        )
    return {
        "eligible": not gaps,
        "gaps": gaps,
        "scope": "This gate permits diagnostic recommendations only; it does not execute experiments",
    }


def branch_context(directory):
    path = directory / "topMisPredicts.csv"
    if not path.exists():
        return {
            "status": "unavailable",
            "unknown": "No branch-PC misprediction export",
        }
    counts, errors = Counter(), []
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        if not {"control_pc", "count"} <= set(reader.fieldnames or []):
            return {
                "status": "unsupported_columns",
                "columns": reader.fieldnames,
            }
        for line, row in enumerate(reader, 2):
            try:
                pc, count = int(row["control_pc"], 16), int(row["count"])
                if pc < 0 or count < 0:
                    raise ValueError("negative value")
                counts[pc] += count
            except (ValueError, TypeError):
                errors.append(
                    {"line": line, "status": "malformed_pc_or_count"}
                )
    return {
        "status": "partial" if errors else "available",
        "source": str(path),
        "population_branch_pcs": len(counts),
        "total_whole_run_mispredictions": sum(counts.values()),
        "top10": [
            {"PC": pc, "count": n}
            for pc, n in sorted(
                counts.items(), key=lambda item: (-item[1], item[0])
            )[:10]
        ],
        "parse_errors": errors[:10],
        "parse_errors_omitted": max(0, len(errors) - 10),
        "window": "whole_run; not aligned with final measurement ROI",
        "scope": (
            "Aggregate control PC across fetch-start PCs; hotspot leads only, "
            "not ROI recovery-cycle attribution"
        ),
    }


def observed_pairs(events, lo, hi):
    opened, pairs = {}, []
    for event in sorted(events, key=lambda e: (e["Tick"], e["ID"])):
        token = (event.get("Attempt"), event.get("Detail"))
        if event.get("Event") == "wait_begin":
            opened[token] = event
        elif event.get("Event") == "wake":
            compatible = next(
                (
                    key
                    for key, begin in opened.items()
                    if key[0] is not None
                    and key[0] == event.get("Attempt")
                    and event.get("Detail")
                    in WAIT_WAKE_SOURCES.get(begin.get("Detail"), set())
                    and (
                        begin.get("Detail") != "stlf"
                        or (
                            begin.get("RelatedSeq")
                            and begin.get("RelatedSeq")
                            == event.get("RelatedSeq")
                        )
                    )
                ),
                None,
            )
            if compatible is None:
                continue
            begin = opened.pop(compatible)
            overlap = max(0, min(event["Tick"], hi) - max(begin["Tick"], lo))
            if overlap:
                pairs.append(
                    {
                        "reason": begin["Detail"],
                        "begin_id": begin["ID"],
                        "wake_id": event["ID"],
                        "Attempt": begin.get("Attempt"),
                        "overlap_ticks": overlap,
                    }
                )
    return pairs


def gather_evidence(diagnosis):
    waits, rejects, dependencies, stores, prefetch = {}, {}, {}, {}, {}
    iq_events = {}
    for example in diagnosis.get("examples", []):
        route = example.get("route", {})
        evidence = route.get("evidence", {})
        key = evidence.get("identity", route.get("identity", {}))
        identity = (key.get("Cpu"), key.get("TID"), key.get("SeqNum"))
        if not identity[2]:
            continue
        window = evidence.get("seed_window", {})
        lo, hi = window.get("start_tick"), window.get("end_tick")
        if lo is None or hi is None or hi <= lo:
            continue
        events = evidence.get("events", [])
        instruction = evidence.get("instruction") or {}
        for pair in observed_pairs(events, lo, hi):
            token = (
                identity,
                pair["reason"],
                pair["begin_id"],
                pair["wake_id"],
            )
            waits[token] = dict(
                pair,
                identity=key,
                source=example.get("source"),
                selection=example.get("selection"),
                PC=instruction.get("PC"),
                evidence_truncated=evidence.get("events_truncated", False),
            )
        for event in events:
            if event.get("Event") == "dependency" and event.get("RelatedSeq"):
                dependencies[(identity, event["ID"])] = {
                    "identity": key,
                    "event_id": event["ID"],
                    "producer_seq": event["RelatedSeq"],
                    "PC": instruction.get("PC"),
                    "detail": event.get("Detail"),
                }
            if (
                event.get("Event") == "store_data_ready"
                or (
                    event.get("Event") == "attempt_begin"
                    and event.get("Detail") == "store_address"
                )
                or (
                    event.get("Event") == "dependency"
                    and "role=store_address"
                    in event.get("Detail", "").split(";")
                )
            ):
                stores[identity] = {
                    "identity": key,
                    "event_id": event["ID"],
                    "PC": instruction.get("PC"),
                    "kind": event["Event"],
                    "source": example.get("source"),
                }
            arbitration_failed = event.get(
                "Event"
            ) == "iq_arbitration" and any(
                reason in event.get("Detail", "").split(";")
                for reason in ("reason=failed", "reason=canceled")
            )
            if lo <= event["Tick"] < hi and (
                event.get("Event")
                in (
                    "iq_issue_blocked",
                    "iq_select_blocked",
                    "iq_cancel",
                    "iq_ready_blocked",
                )
                or arbitration_failed
            ):
                iq_events[(identity, event["ID"])] = {
                    "identity": key,
                    "event_id": event["ID"],
                    "Tick": event["Tick"],
                    "Event": event["Event"],
                    "detail": event.get("Detail"),
                    "PC": instruction.get("PC"),
                    "source": example.get("source"),
                }
        cache = route.get("cache", {})
        for event in cache.get("events", []):
            if (
                event.get("Event") == "reject"
                and lo <= event["Tick"] < hi
                and event.get("BlockedMask", 0) & 1
                and event.get("Cache") == "system.cpu.dcache"
                and event.get("Detail") == "blocked"
            ):
                rejects[event["ID"]] = {
                    "identity": key,
                    "event_id": event["ID"],
                    "RequestID": event.get("RequestID"),
                    "Cache": event.get("Cache"),
                    "detail": event.get("Detail"),
                    "BlockedMask": event["BlockedMask"],
                    "source": example.get("source"),
                    "selection": example.get("selection"),
                }
        for snapshot in cache.get("rejection_owners", []):
            parent = snapshot.get("reject_id")
            if parent not in rejects:
                continue
            for owner in snapshot.get("events", []):
                if (
                    owner.get("Event") == "target_owner"
                    and owner.get("RequestID")
                    and "HardPFReq" in owner.get("Detail", "")
                ):
                    prefetch[(identity, owner["RequestID"])] = {
                        "identity": key,
                        "reject_id": parent,
                        "owner_event_id": owner["ID"],
                        "RequestID": owner["RequestID"],
                    }
    return {
        "waits": list(waits.values()),
        "rejects": list(rejects.values()),
        "dependencies": list(dependencies.values()),
        "stores": list(stores.values()),
        "iq_events": list(iq_events.values()),
        "prefetch_owners": list(prefetch.values()),
    }


def distinct_instances(records):
    return len(
        {
            (
                r["identity"].get("Cpu"),
                r["identity"].get("TID"),
                r["identity"].get("SeqNum"),
            )
            for r in records
        }
    )


def stat_count(stats, name):
    try:
        value = float(stats[name])
        return int(value) if value >= 0 and value.is_integer() else None
    except (KeyError, ValueError, OverflowError):
        return None


def config_values(path):
    """Read only needed sections; gem5 embeds multiline SQL outside INI syntax."""
    wanted = {
        "system.cpu.dcache",
        "system.cpu.mmu.dtb",
        "system.cpu.dcache.prefetcher",
    }
    sections, current = {}, None
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.strip()
            if line.startswith("[") and line.endswith("]"):
                current = line[1:-1]
            elif current in wanted and "=" in line:
                name, value = line.split("=", 1)
                sections.setdefault(current, {})[name] = value
    return sections


def config_integer(config, section, name):
    try:
        return int(config.get(section, {})[name])
    except (KeyError, ValueError):
        return 0


def primary_dispatch(diagnosis):
    dispatch = diagnosis.get("dispatch_stall_reasons", {})
    bins = dispatch.get("top_non_nostall", [])
    if (
        dispatch.get("status") != "available"
        or not dispatch.get("bins_match_total")
        or not bins
    ):
        return None
    # The commit-first rule is an existing investigation hint, not a gain threshold.
    if (
        diagnosis.get("investigation_hints", {})
        .get("suggested_start", {})
        .get("next_entry")
        == "commit_blocker"
    ):
        return None
    return bins[0]


def main_categories(primary):
    reason = primary["reason"] if primary else ""
    if reason.startswith("Store"):
        return {"store_dispatch_or_completion"}
    if reason.startswith("Load"):
        return {
            "actual_mshr_admission_reject",
            "repeated_cache_refill_wait",
            "refill_tag_write_contention",
            "memory_admission_gap",
        }
    if reason == "DTlbStall":
        return {"store_or_load_translation"}
    if reason in (
        "ScalarLongExecute",
        "VectorLongExecute",
        "InstNotReady",
        "ROBFull",
        "RegFull",
    ):
        return {"iq_dependency_candidates"}
    if reason.endswith("Recovery") or reason == "CommitSquash":
        return {"control_recovery"}
    if reason.startswith("Fetch") or reason == "InstMisPred":
        return {"frontend_supply_gap"}
    return set()


def choose_candidates(candidates, primary, limit=3):
    """Reserve the main investigation entry, then favor direct and repeated local observations."""
    preferred = main_categories(primary)
    levels = {
        "observed_blocker_mechanism": 0,
        "observed_instruction_mechanism": 1,
        "relation_or_counter_lead": 2,
    }
    local = sorted(
        candidates,
        key=lambda c: (
            levels[c["selection_evidence"]["evidence_level"]],
            -c["selection_evidence"]["distinct_instances"],
            c["category"],
        ),
    )
    main = next((c for c in local if c["category"] in preferred), None)
    ordered = ([main] if main else []) + [c for c in local if c is not main]
    selected = ordered[:limit]
    audit = [
        {
            "category": c["category"],
            "retained": c in selected,
            "main_dispatch_match": c["category"] in preferred,
            **c["selection_evidence"],
        }
        for c in ordered
    ]
    for candidate in selected:
        candidate["selection_reason"] = (
            "main_dispatch_investigation_entry"
            if candidate is main
            else "direct_local_evidence_then_repetition"
        )
    return selected, audit


def local_level(records):
    if any(
        r.get("source") in ("commit_blocker", "partial_commit_blocker")
        for r in records
    ):
        return "observed_blocker_mechanism"
    return (
        "observed_instruction_mechanism"
        if records
        else "relation_or_counter_lead"
    )


def analyze(case_dir, no_interventions=False):
    diagnosis = read_json(case_dir / "diagnose.json", {})
    validation = read_json(case_dir / "validation.json", {})
    commands = {
        side: read_json(case_dir / side / "command.json", {})
        for side in ("off", "on")
    }
    gate = execution_gate(
        validation, commands["off"], commands["on"], diagnosis
    )
    stats_path = case_dir / "off" / "stats.txt"
    stats = final_stats(stats_path) if stats_path.exists() else {}
    config = config_values(case_dir / "off" / "config.ini")
    roi = diagnosis.get("query_window", {})
    if stats and (
        str(roi.get("end_tick")) != stats.get("finalTick")
        or roi.get("start_tick")
        != int(stats["finalTick"]) - int(stats["simTicks"])
    ):
        gate["gaps"].append(
            "diagnosis ROI does not match the final OFF measurement statistics"
        )
        gate["eligible"] = False
    branches = branch_context(case_dir / "off")
    evidence = gather_evidence(diagnosis)
    translation = [
        w for w in evidence["waits"] if w["reason"] == "translation"
    ]
    refill = [w for w in evidence["waits"] if w["reason"] == "cache_refill"]
    primary = primary_dispatch(diagnosis)
    iq_symptoms = [
        r
        for r in diagnosis.get("symptom_rankings", [])
        if r.get("iq_to_fu", 0)
        > max(r.get("execution_or_memory", 0), r.get("rob_drain", 0))
    ]
    candidates, interventions = [], []

    def add(
        category,
        facts,
        mechanism,
        unknowns,
        artifact,
        records=(),
        level=None,
        proposal=None,
    ):
        item = {
            "category": category,
            "facts": facts,
            "local_mechanism": mechanism,
            "unknowns": unknowns,
            "root_cause_status": "not_established",
            "next_probe": {"kind": "artifact_request", "request": artifact},
            "selection_evidence": {
                "evidence_level": level or local_level(records),
                "distinct_instances": distinct_instances(records),
            },
            "_proposal": proposal,
        }
        candidates.append(item)

    if evidence["rejects"]:
        baseline = config_integer(config, "system.cpu.dcache", "mshrs")
        blocked = stat_count(
            stats, "system.cpu.dcache.blockedCauses::no_mshrs"
        )
        reject_proof = [
            r
            for r in evidence["rejects"]
            if r["source"] in ("commit_blocker", "partial_commit_blocker")
        ]
        repeated = distinct_instances(reject_proof)
        proposal = (
            {
                "id": "l1d_mshrs_x2",
                "parameter": "system.cpu[0].dcache.mshrs",
                "value": baseline * 2,
                "basis": "多个动态请求的实际 NoMSHR 拒绝与 measurement 阻塞计数相互支撑",
                "target_counter": "system.cpu.dcache.blockedCauses::no_mshrs",
                "diagnostic_scope": "仅测试模型 MSHR 容量敏感性；不是实际硬件新增条目的收益估计",
                "evidence": {
                    "actual_no_mshr_reject_ids": [
                        r["event_id"] for r in reject_proof
                    ],
                    "rejected_instances": [
                        r["identity"] for r in reject_proof
                    ],
                    "measurement_no_mshr_blocked_causes": blocked,
                },
            }
            if baseline > 0 and blocked and repeated >= 2
            else None
        )
        add(
            "actual_mshr_admission_reject",
            {
                "rejects": evidence["rejects"],
                "instances": distinct_instances(evidence["rejects"]),
            },
            "真实 admission 请求被拒绝时 NoMSHRs 位已置位；owner 快照只表示候选持有者",
            ["资源释放/credit 归还时间线与可恢复运行时间仍未知", "单个拒绝实例或仅有 blocked 状态不准入容量实验"],
            "沿 ParentID 查询实际 owner 与 MSHR/credit 释放；补多个动态实例及测量段阻塞计数",
            records=evidence["rejects"],
            proposal=proposal,
        )
    if translation:
        repeated = distinct_instances(translation)
        proof = [
            r
            for r in translation
            if r["source"] in ("commit_blocker", "partial_commit_blocker")
        ]
        size = config_integer(config, "system.cpu.mmu.dtb", "size")
        misses = stat_count(stats, "system.cpu.mmu.dtb.misses")
        identities = {}
        for record in proof:
            key = record["identity"]
            identities[
                (key.get("Cpu"), key.get("TID"), key.get("SeqNum"))
            ] = dict(key, Attempt=record["Attempt"])
        proposal = (
            {
                "id": "dtlb_x4",
                "parameter": "system.cpu[0].mmu.dtb.size",
                "value": size * 4,
                "basis": "多个实际 blocker 的 translation 区间与 measurement DTLB miss 相互支撑",
                "target_counter": "system.cpu.mmu.dtb.misses",
                "diagnostic_scope": "仅提供模型容量敏感性实验建议，不自动执行、不代表真实硬件收益",
                "evidence": {
                    "repeated_translation_instances": list(
                        identities.values()
                    ),
                    "measurement_dtlb_misses": misses,
                    "pairs": proof,
                },
            }
            if len(identities) >= 2 and size > 0 and misses
            else None
        )
        add(
            "store_or_load_translation",
            {
                "observed_pairs": translation,
                "distinct_instances": repeated,
                "measurement_dtlb_misses": stats.get(
                    "system.cpu.mmu.dtb.misses"
                ),
            },
            "同 Attempt 且 wake source 兼容的 translation 区间与选中调查窗口相交",
            ["translation 等待不直接等同 DTLB miss；容量、冷 miss、PTW 竞争与翻译缓存延迟尚未区分"],
            "补时间分散的多个 translation 实例及测量段 PTW/TLB miss 计数",
            records=translation,
            proposal=proposal,
        )
    dispatch = diagnosis.get("dispatch_stall_reasons", {})
    recovery = [
        b
        for b in dispatch.get("top_non_nostall", [])
        if b.get("reason") == "ControlRecovery"
    ]
    if recovery:
        add(
            "control_recovery",
            {"dispatch_slots": recovery, "branch_hotspots": branches},
            "IEW 已观察到 ControlRecovery；全运行 branch 热点只提供进一步调查的地址",
            ["branch PC 次数没有与 measurement 对齐；逐分支恢复 epoch 和资源占用链缺失"],
            "在热点 branch PC 采有限 branch/squash epoch，并与 measurement ROI 对齐",
        )
    if refill:
        add(
            "repeated_cache_refill_wait",
            {
                "observed_pairs": refill,
                "distinct_instances": distinct_instances(refill),
                "measurement_demand_merged_pf": stats.get(
                    "system.cpu.dcache.demandMergedIntoPfMSHR"
                ),
                "measurement_admission_counters": {
                    key: stats.get(key)
                    for key in (
                        "system.cpu.dcache.blockedCauses::no_mshrs",
                        "system.cpu.dcache.blockedCauses::no_targets",
                    )
                },
            },
            "兼容的 refill wait/wake 区间与选中窗口相交；cache_hint 不是数据响应",
            [
                "下层 cache/DRAM 身份与普通 miss、refresh、服务和队列延迟尚未区分",
                "demand 合并预取 MSHR 本身不能证明预取有害",
            ],
            "追典型实例 RequestID/MSHR/target 的下层关联，不只调查最长 span",
            records=refill,
        )
    if evidence["stores"] or (
        primary and primary["reason"].startswith("Store")
    ):
        counters = {
            key: stats.get(key)
            for key in (
                "system.cpu.iew.stallEvents::IQFull",
                "system.cpu.numCycles",
                "system.cpu.lsq.sbufferFullCycles",
                "system.cpu.lsq0.sbufferFull",
                "system.cpu.lsq0.storeReplayPhysicalSQFull",
                "system.cpu.lsq0.storePhysicalSQReplayBlocked",
                "system.cpu.scheduler.std0.avgInsts",
                "system.cpu.scheduler.std1.avgInsts",
            )
        }
        add(
            "store_dispatch_or_completion",
            {
                "store_events": evidence["stores"],
                "primary_dispatch": primary,
                "measurement_counters": counters,
                "upstream_iq_context": {
                    "iq_dominant_symptom_pcs": iq_symptoms,
                    "scope": "Stage evidence; queue owner unknown",
                },
            },
            "store dispatch 或提交进度值得调查；已观察到的 STA/STD 时间戳并不定位资源根因",
            [
                "StoreL1Bound 可由 ROB head 已 issued 且没有 pendingCacheReq 推断，不等于 L1 或 StoreBuffer 根因",
                "IQFull 可因 scheduler 容量/输入带宽失败再传播到所有 dispatch lanes；须区分 STD 队列与 SB 占用",
                "STA/STD、group readiness 和执行入口尚未形成完整阻塞链",
            ],
            "先查询 STD 队列/入口失败和实际 group readiness；不据 StoreL1Bound 自动放大 StoreBuffer",
            records=evidence["stores"],
            level="relation_or_counter_lead",
        )
    tag_writes = [
        r
        for r in evidence["iq_events"]
        if "reason=replay_refill_tag_write" in r.get("detail", "")
    ]
    if tag_writes:
        add(
            "refill_tag_write_contention",
            {
                "window_events": tag_writes,
                "distinct_instances": distinct_instances(tag_writes),
            },
            "实际 replay 发射检查因 refill tag 写受阻；这是跨 cache/LSQ 的局部资源观察",
            ["事件次数不是阻塞持续周期，也不能解释所有 bank replay 或零提交 spans"],
            "查询 tag 写占用者、replay 下一次实际 issue 与提交 blocker 的交叠",
            records=tag_writes,
        )
    iq_events = [r for r in evidence["iq_events"] if r not in tag_writes]
    if (
        evidence["dependencies"]
        or iq_events
        or (primary and "iq_dependency_candidates" in main_categories(primary))
    ):
        add(
            "iq_dependency_candidates",
            {
                "relations": evidence["dependencies"][:10],
                "relations_omitted": max(
                    0, len(evidence["dependencies"]) - 10
                ),
                "window_decisions": iq_events,
                "iq_dominant_symptom_pcs": iq_symptoms,
            },
            "保留 producer 关系与窗口内实际 IQ 选择/取消/受阻观察；不识别唯一 critical predecessor",
            ["阶段累计 instruction-ticks 不是运行时间；最早操作数可用时间、端口预约和取消仍待区分"],
            "查询 producer writeback、IQ selection/cancel 与 ready-to-issue；不猜测 FU 延迟",
            records=iq_events,
        )
    retained, selection = choose_candidates(candidates, primary)
    for candidate in retained:
        proposal = candidate.pop("_proposal")
        if (
            proposal
            and not no_interventions
            and gate["eligible"]
            and len(interventions) < 2
        ):
            interventions.append(proposal)
            candidate["next_probe"] = {
                "kind": "single_parameter_comparison",
                "intervention_id": proposal["id"],
                "parameter": proposal["parameter"],
                "value": proposal["value"],
                "baseline": "existing OFF; same binary/REF and 5M+5M",
                "trace": "OFF",
            }
    kept_categories = {c["category"] for c in retained}
    return {
        "slice": case_dir.name,
        "execution_gate": gate,
        "candidates": retained,
        "primary_dispatch": primary,
        "candidate_selection_audit": selection,
        "selection_scope": (
            "Main dispatch entry is an investigation hint. "
            "Local evidence levels order observations, "
            "not root-cause confidence. Instance counts cover returned representatives only, "
            "not population frequency; units are not added across rankings"
        ),
        "omitted_candidate_categories": [
            c["category"]
            for c in candidates
            if c["category"] not in kept_categories
        ],
        "interventions": interventions,
        "branch_context": branches,
        "selection_audits": diagnosis.get(
            "selection_audits", {"status": "unavailable_old_diagnosis"}
        ),
        "scope": (
            "At most three candidates and two diagnostic recommendations; "
            "no automatic root-cause or gain estimate"
        ),
        "intervention_recommendations_enabled": not no_interventions,
        "prefetch_gap": "No prefetch experiment is recommended without repeated request-linked evidence",
        "recommendation_limit_per_case": 2,
    }


def markdown(result):
    lines = [
        f"# {result['slice']}：候选与下一验证",
        "",
        "候选来自有界动态证据。没有估算可恢复周期，容量干预只用于模型敏感性诊断。",
        "",
        f"实验建议证据检查：{'通过' if result['execution_gate']['eligible'] else '未通过'}。",
        f"本次建议：{'启用' if result['intervention_recommendations_enabled'] else '关闭'}；脚本不执行模拟器。",
    ]
    if result["execution_gate"]["gaps"]:
        lines += ["", "缺失条件："] + [
            f"- {gap}" for gap in result["execution_gate"]["gaps"]
        ]
    for candidate in result["candidates"]:
        lines += [
            "",
            f"## {candidate['category']}",
            "",
            "已观测事实：",
            "",
            "```json",
            json.dumps(candidate["facts"], indent=2, ensure_ascii=False),
            "```",
            "",
            "局部机制：" + candidate["local_mechanism"],
            "",
            "未知环节：",
        ]
        lines += ["- " + gap for gap in candidate["unknowns"]]
        lines += [
            "",
            "下一步：" + json.dumps(candidate["next_probe"], ensure_ascii=False),
        ]
    lines += ["", "分支 CSV 覆盖整个运行，不能当作测量段逐分支恢复周期；TopN 审计与各类观测量分母也不可相加。", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case-dir", type=Path, required=True)
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Write reports separately from the read-only input case",
    )
    parser.add_argument(
        "--no-interventions",
        action="store_true",
        help="Emit artifact requests only",
    )
    args = parser.parse_args()
    result = analyze(args.case_dir.resolve(), args.no_interventions)
    output = args.output_dir or args.case_dir
    output.mkdir(parents=True, exist_ok=True)
    (output / "candidates.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n"
    )
    (output / "CANDIDATES.md").write_text(markdown(result))
    print(
        json.dumps(
            {
                "slice": result["slice"],
                "candidates": len(result["candidates"]),
                "interventions": len(result["interventions"]),
                "gate": result["execution_gate"]["eligible"],
            }
        )
    )


if __name__ == "__main__":
    main()
