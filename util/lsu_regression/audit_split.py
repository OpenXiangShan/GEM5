#!/usr/bin/env python3
"""Audit the exact definition partition of the standalone LSQUnit split.

Compare against a pinned Git base without editing source or headers. This
baseline-specific lexer is a bounded source audit, not a full C++ parser.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

LEX = re.compile(
    r'//[^\n]*|/\*[\s\S]*?\*/|R"(?P<delim>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=delim)"|'
    r'"(?:\\[\s\S]|[^"\\])*"|'
    r"'(?:\\[\s\S]|[^'\\])*'|[A-Za-z_][A-Za-z_0-9]*|"
    r"(?:\d[\w.]*|::|->|<<|>>|&&|\|\||==|!=|<=|>=|\+\+|--|\S)"
)
GROUPS = {
    "core": """LSQUnit tick init resetState name LSQUnitStats::LSQUnitStats
        setDcachePort drainSanityCheck takeOverFrom executePipeSx dumpInsts
        schedule getMMUPtr cacheLineSize""".split(),
    "queue": """SQEntry::setStatus insert insertLoad insertStore currentLoadRequest
        currentStoreRequest splitStoreAddrSquashed numFreeLoadEntries
        numFreeStoreEntries getAndResetLastClockLQPopEntries
        getAndResetLastClockSQPopEntries storeQueueWriteReady recordAddrOrDataReady
        recordAddrOrDataDequeue recordPhysicalSQReplayBlocked recordStoreQueueReplay
        commitLoad commitLoads commitStores hasStoresToWBBefore getLoadHeadSeqNum
        getStoreHeadSeqNum""".split(),
    "load_pipe": """issueToLoadPipe loadDoTranslate loadDoSendRequest loadDoRecvData
        loadDoWriteback executeLoadPipeSx""".split(),
    "store_pipe": """issueToStorePipe storeDoTranslate storeDoWriteSQ emptyStorePipeSx
        executeStorePipeSx executeAmo triggerStorePFTrain write writebackBlockedStore
        directStoreToCache storePostSend countStoreBufferOffloadableEntries
        offloadToStoreBuffer insertStoreBuffer""".split(),
    "forward": "checkStoreLoadForwardingRange makeFullFwdPkt forwardFromBus read".split(),
    "order": """pipeLineNukeCheck getMemDepViolator checkSnoop checkLocalStoreVisible
        checkViolations overlapsVisibleStore""".split(),
    "replay": """loadSetReplay storeSetReplay bankConflictReplay tagReadFailReplay
        bankConflictReplaySchedule tagReadFailReplaySchedule
        bankConflictReplayEvent::bankConflictReplayEvent bankConflictReplayEvent::process
        bankConflictReplayEvent::description tagReadFailReplayEvent::tagReadFailReplayEvent
        tagReadFailReplayEvent::process tagReadFailReplayEvent::description
        addToRARReplayQueue addToRAWReplayQueue processReplayQueues updateCompletedIdx""".split(),
    "completion": """WritebackRegEvent::WritebackRegEvent WritebackRegEvent::process
        WritebackRegEvent::description recvTimingResp completeDataAccess
        writebackReg completeStore""".split(),
    "recovery": "squash getLatestHtmUid".split(),
    "cache": "trySendPacket recvRetry startStaleTranslationFlush checkStaleTranslations".split(),
}
OWNER = {
    (
        "LSQUnit::" + symbol if symbol != "overlapsVisibleStore" else symbol
    ): group
    for group, symbols in GROUPS.items()
    for symbol in symbols
}
PHASES = {
    "R01": set(),
    "R02": {"queue", "order"},
    "R03": {"queue", "order", "load_pipe", "forward", "replay"},
    "R04": set(GROUPS) - {"core"},
}
PATTERN = re.compile(
    r"(?m)^(?:[\w:<>]+[ \t]+[*&]?[ \t]*)?(LSQUnit(?:::[A-Za-z_]\w*)+|overlapsVisibleStore)\s*\("
)


def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


def lexical_tokens(text):
    return [
        m[0] for m in LEX.finditer(text) if not m[0].startswith(("//", "/*"))
    ]


def masked(text):
    chars = list(text)
    for match in LEX.finditer(text):
        if match[0].startswith(("//", "/*", '"', "'", 'R"')):
            chars[match.start() : match.end()] = [
                "\n" if c == "\n" else " " for c in match[0]
            ]
    return "".join(chars)


def definitions(text, require_complete=True):
    safe = masked(text)
    result = []
    for match in PATTERN.finditer(safe):
        symbol = match[1]
        line_start = text.rfind("\n", 0, match.start()) + 1
        start = line_start
        if not safe[line_start : match.start(1)].strip():
            prior_end = line_start - 1
            prior_start = text.rfind("\n", 0, max(0, prior_end)) + 1
            prior_line = safe[prior_start:prior_end].strip()
            if prior_line and not prior_line.endswith(("}", "{", ";")):
                start = prior_start
        opening = safe.index("{", match.end())
        depth = 0
        ending = None
        for index in range(opening, len(safe)):
            if safe[index] == "{":
                depth += 1
            elif safe[index] == "}":
                depth -= 1
                if depth == 0:
                    ending = index + 1
                    break
        if ending is None:
            raise ValueError("Unclosed definition: " + symbol)
        result.append(
            {
                "symbol": symbol,
                "start": start,
                "end": ending,
                "signature": " ".join(lexical_tokens(text[start:opening])),
                "body_open": opening,
                "line_start": text.count("\n", 0, start) + 1,
                "line_end": text.count("\n", 0, ending) + 1,
            }
        )
    if not result or len({item["signature"] for item in result}) != len(
        result
    ):
        raise ValueError("Empty or duplicate full definition signatures")
    unknown = sorted(set(item["symbol"] for item in result) - set(OWNER))
    missing = (
        sorted(set(OWNER) - set(item["symbol"] for item in result))
        if require_complete
        else []
    )
    if unknown or missing:
        raise ValueError(
            f"Ownership mismatch: unknown={unknown}, missing={missing}"
        )
    return result


def extract(text):
    items = definitions(text)
    prefix = text[: items[0]["start"]]
    cursor = items[0]["start"]
    for item in items:
        chunk_end = item["end"]
        if item["symbol"] == "overlapsVisibleStore":
            # Retain internal linkage by moving its entire anonymous namespace.
            close = text.index("} // anonymous namespace", chunk_end)
            chunk_end = close + len("} // anonymous namespace")
        item["chunk_start"] = cursor
        item["chunk_end"] = chunk_end
        item["chunk"] = text[cursor:chunk_end]
        item["group"] = OWNER[item["symbol"]]
        cursor = chunk_end
    suffix = text[cursor:]
    if prefix + "".join(item["chunk"] for item in items) + suffix != text:
        raise ValueError("Byte partition does not reconstruct baseline")
    return prefix, items, suffix


def filename(group):
    return "lsq_unit.cc" if group == "core" else f"lsq_unit_{group}.cc"


def output_sources(source, phase):
    prefix, items, suffix = extract(source)
    moved = PHASES[phase]
    outputs = {
        "lsq_unit.cc": prefix
        + "".join(
            item["chunk"] for item in items if item["group"] not in moved
        )
        + suffix
    }
    for group in GROUPS:
        if group in moved:
            outputs[filename(group)] = (
                prefix
                + "".join(
                    item["chunk"] for item in items if item["group"] == group
                )
                + suffix
            )
    return outputs, items


def audit(baseline, outputs, headers, repo):
    original = definitions(baseline)
    original_by_sig = {item["signature"]: item for item in original}
    actual = []
    for name, source in outputs.items():
        for item in definitions(source, require_complete=False):
            old = original_by_sig.get(item["signature"])
            current = source[item["start"] : item["end"]]
            before = baseline[old["start"] : old["end"]] if old else ""
            actual.append(
                {
                    "signature": item["signature"],
                    "symbol": item["symbol"],
                    "file": name,
                    "line": item["line_start"],
                    "token_equal": lexical_tokens(current)
                    == lexical_tokens(before),
                    "byte_equal": current == before,
                    "source_definition_sha256": sha(before),
                    "target_definition_sha256": sha(current),
                    "source_token_sha256": sha(
                        json.dumps(lexical_tokens(before))
                    ),
                    "target_token_sha256": sha(
                        json.dumps(lexical_tokens(current))
                    ),
                }
            )
    expected = Counter(original_by_sig.keys())
    found = Counter(item["signature"] for item in actual)
    helper_files = [
        name
        for name, text in outputs.items()
        if "bool\noverlapsVisibleStore(" in text
    ]
    header_equal = {
        name: sha((repo / "src/cpu/o3" / name).read_text()) == sha(text)
        for name, text in headers.items()
    }
    passed = (
        found == expected
        and all(x["token_equal"] and x["byte_equal"] for x in actual)
        and all(header_equal.values())
        and len(helper_files) == 1
        and all(
            not re.search(r'#include\s+["<][^">]*\.cc[">]', text)
            for text in outputs.values()
        )
    )
    return {
        "passed": passed,
        "baseline_definition_count": len(original),
        "output_definition_count": len(actual),
        "missing": list((expected - found).elements()),
        "extra": list((found - expected).elements()),
        "headers_unchanged": header_equal,
        "all_definition_tokens_identical": all(
            x["token_equal"] for x in actual
        ),
        "all_definition_bytes_identical": all(x["byte_equal"] for x in actual),
        "helper_unique_file": helper_files,
        "anonymous_namespace_count": sum(
            source.count("} // anonymous namespace")
            for source in outputs.values()
        ),
        "include_cc_count": sum(
            len(re.findall(r'#include\s+["<][^">]*\.cc[">]', source))
            for source in outputs.values()
        ),
        "definitions": actual,
    }


def main():
    import subprocess

    parser = argparse.ArgumentParser(
        description="Audit the exact LSQUnit implementation partition"
    )
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--base", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = args.repo.resolve()

    def original(name):
        return subprocess.check_output(
            ["git", "show", args.base + ":src/cpu/o3/" + name], cwd=repo
        ).decode()

    source = original("lsq_unit.cc")
    headers = {n: original(n) for n in ["lsq.hh", "lsq_unit.hh"]}
    generated, _ = output_sources(source, "R04")
    actual = {n: (repo / "src/cpu/o3" / n).read_text() for n in generated}
    report = audit(source, actual, headers, repo)
    report["generated_source_bytes_match"] = generated == actual
    build = (repo / "src/cpu/o3/SConscript").read_text()
    report["build_registration_unique"] = all(
        build.count("Source('" + n + "')") == 1 for n in actual
    )
    report["passed"] &= (
        report["generated_source_bytes_match"]
        and report["build_registration_unique"]
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print({k: v for k, v in report.items() if k != "definitions"})
    raise SystemExit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()
