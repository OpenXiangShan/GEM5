#!/usr/bin/env python3
"""
Generate a "since fork-point" upstream report for this GEM5 repo.

This script is intentionally self-contained (no external deps beyond pandas/matplotlib).
It extracts commit metadata + numstat in bulk via `git log`, then produces:
  - Markdown reports (overview + per-commit list)
  - CSV datasets for further analysis
  - Simple plots (commit volume, arch activity, etc.)
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable, Optional

import matplotlib.pyplot as plt
import pandas as pd


RECORD_SEP = "\x1e"
FIELD_SEP = "\x1f"


def run_git(repo: Path, args: list[str]) -> str:
    # Capture stderr to avoid polluting generated files/CLI with non-fatal git warnings
    # (e.g., annotated tag internal-name mismatches in upstream repos).
    return subprocess.check_output(
        ["git", *args],
        cwd=repo,
        text=True,
        errors="replace",
        stderr=subprocess.PIPE,
    )


def git_rev_parse(repo: Path, ref: str) -> str:
    return run_git(repo, ["rev-parse", ref]).strip()


def git_merge_base(repo: Path, a: str, b: str) -> str:
    return run_git(repo, ["merge-base", a, b]).strip()


def git_describe_tag(repo: Path, commit: str) -> str:
    """
    Best-effort: return a *version-like* tag name (e.g., v25.1.0.0) for a ref/commit.

    We avoid relying on annotated-tag "internal names" (which can differ from the ref
    name and trigger warnings). Preference order:
      1) Tags exactly pointing at the commit (choose max version).
      2) Fallback to `git describe` and strip the `-<n>-g<sha>` suffix.
    """

    def parse_ver(tag: str) -> Optional[tuple[int, ...]]:
        if not tag.startswith("v"):
            return None
        parts = tag[1:].split(".")
        if not parts or not all(p.isdigit() for p in parts):
            return None
        return tuple(int(p) for p in parts)

    try:
        c = git_rev_parse(repo, commit)
    except subprocess.CalledProcessError:
        c = commit

    try:
        tags = [t.strip() for t in run_git(repo, ["tag", "--points-at", c]).splitlines() if t.strip()]
        vtags = [t for t in tags if parse_ver(t)]
        if vtags:
            return max(vtags, key=lambda t: parse_ver(t) or (0,))
    except subprocess.CalledProcessError:
        pass

    try:
        desc = run_git(repo, ["describe", "--tags", "--match", "v[0-9]*", "--always", c]).strip()
        # Typical: v25.1.0.0-36-g<sha>
        return desc.split("-", 1)[0]
    except subprocess.CalledProcessError:
        return ""


@dataclass(frozen=True)
class CommitMeta:
    commit: str
    parents: list[str]
    author_name: str
    author_email: str
    date_iso: str
    subject: str

    @property
    def is_merge(self) -> bool:
        return len(self.parents) > 1

    @property
    def date(self) -> datetime:
        # git iso-strict: 2025-12-31T13:13:38-08:00
        return datetime.fromisoformat(self.date_iso)

    @property
    def ym(self) -> str:
        d = self.date
        return f"{d.year:04d}-{d.month:02d}"

    @property
    def ymd(self) -> str:
        d = self.date
        return f"{d.year:04d}-{d.month:02d}-{d.day:02d}"

    @property
    def short(self) -> str:
        return self.commit[:12]


def topic_prefix(subject: str) -> str:
    # gem5 often uses "topic: message". Keep a reasonable prefix bucket.
    if ":" not in subject:
        return "no-prefix"
    prefix = subject.split(":", 1)[0].strip()
    if not prefix or len(prefix) > 32:
        return "other"
    return prefix


def parse_version_tuple(s: str) -> Optional[tuple[int, ...]]:
    """
    Parse version-like strings:
      - v25.1.0.0  -> (25, 1, 0, 0)
      - 25.1       -> (25, 1)
    """
    t = s.strip()
    if t.startswith("v"):
        t = t[1:]
    parts = t.split(".")
    if not parts or not all(p.isdigit() for p in parts):
        return None
    return tuple(int(p) for p in parts)


def extract_pr_number(subject: str) -> Optional[int]:
    m = re.search(r"\(#(\d+)\)\s*$", subject)
    if m:
        try:
            return int(m.group(1))
        except ValueError:
            return None
    return None


def extract_pr_number_any(subject: str, body: str = "") -> Optional[int]:
    """
    Best-effort PR number extraction.
    Supports:
      - "... (#1234)"
      - "Merge pull request #1234 from ..."
      - URLs in subject/body: github.com/gem5/gem5/pull/1234
    """
    pr = extract_pr_number(subject)
    if pr is not None:
        return pr

    m = re.search(r"Merge pull request #(\d+)\b", subject)
    if m:
        try:
            return int(m.group(1))
        except ValueError:
            return None

    m = re.search(r"github\.com/gem5/gem5/pull/(\d+)", subject + "\n" + (body or ""))
    if m:
        try:
            return int(m.group(1))
        except ValueError:
            return None

    return None


def clean_pr_title(subject: str, pr: Optional[int], body: str = "") -> str:
    """
    Generate a more PR-like title from a commit subject/body.
    - Strip trailing "(#PR)" when present.
    - For GitHub merge commits, prefer the first non-empty body line as the PR title.
    """
    s = subject.strip()
    if pr is not None:
        # GitHub merge commit title is often not descriptive.
        if s.lower().startswith("merge pull request"):
            for ln in (body or "").splitlines():
                ln = ln.strip()
                if ln:
                    return ln
        # Strip the common "(#1234)" PR suffix.
        s = re.sub(rf"\s*\(#\s*{pr}\s*\)\s*$", "", s).strip()
    return s


def _rough_en_to_zh(text: str) -> str:
    """
    Best-effort EN->ZH for short technical titles.
    We intentionally keep technical tokens, filenames, and acronyms.

    This is *not* a full translator; it's a readability helper for scanning.
    """

    s = (text or "").strip()
    if not s:
        return ""

    # Common phrase replacements (case-insensitive).
    # Order matters: longer phrases first.
    repl: list[tuple[str, str]] = [
        (r"\bnew\b", "新增"),
        (r"\bimproved\b", "改进"),
        (r"\bimprovements\b", "改进"),
        (r"\bdecoupled\b", "解耦"),
        (r"\bdistributed\b", "分布式"),
        (r"\bconfigurable\b", "可配置"),
        (r"\bmultiple\b", "多"),
        (r"\band\b", "与"),
        (r"\btowards\b", "推进"),
        (r"\bfull\b", "完整"),
        (r"\boptional\b", "可选"),
        (r"\bsupports?\b", "支持"),
        (r"\bimplementation\b", "实现"),
        (r"\bbehavior\b", "行为"),
        (r"\bnon-serializing\b", "非序列化"),
        (r"\bregisters\b", "寄存器"),
        (r"\binfrastructure\b", "基础设施"),
        (r"\bmachinery\b", "机制"),
        (r"\bGPU memory size\b", "GPU 显存大小"),
        (r"\bmemory size\b", "内存大小"),
        (r"\bsystem calls?\b", "系统调用"),
        (r"\bcore model\b", "核心模型"),
        (r"\btable[- ]walk\b", "页表遍历"),
        (r"\binstruction[- ]queue\b", "指令队列"),
        (r"\bissue queue\b", "发射队列"),
        (r"\bfront end\b", "前端"),
        (r"\bback end\b", "后端"),
        (r"\bfix(es|ed)?\b", "修复"),
        (r"\bremove(d|s)?\b", "移除"),
        (r"\badd(s|ed)?\b", "新增"),
        (r"segmentation fault", "段错误"),
        (r"memory leak(age)?", "内存泄漏"),
        (r"compile error", "编译错误"),
        (r"build error", "构建错误"),
        (r"assert(ion)? fail(ure)?", "断言失败"),
        (r"\bdeadlock\b", "死锁"),
        (r"\bhang(s|ing)?\b", "卡住"),
        (r"\bstuck\b", "卡住"),
        (r"\bcrash(es)?\b", "崩溃"),
        (r"\bworkload(s)?\b", "工作负载"),
        (r"\bdocumentation\b", "文档"),
        (r"\bunit tests?\b", "单元测试"),
        (r"\bprefetcher(s)?\b", "预取器"),
        (r"\bbranch predictor(s)?\b", "分支预测器"),
        (r"\bstatistics\b", "统计"),
        (r"\bperformance\b", "性能"),
    ]
    for pat, rep in repl:
        s = re.sub(pat, rep, s, flags=re.IGNORECASE)

    # Normalize some connectors for later pattern matching.
    s = re.sub(r"\s+", " ", s).strip()
    # Chinese typically doesn't use spaces; remove spaces between CJK chars for readability.
    s = re.sub(r"([\u4e00-\u9fff])\s+([\u4e00-\u9fff])", r"\1\2", s)
    return s


def pr_one_liner_zh(title: str, action: str) -> str:
    """
    Produce a single-sentence Chinese explanation for a PR, based on its title.
    We keep the original scope prefix if present, but translate the "verb phrase"
    to improve readability.
    """

    raw = (title or "").strip()
    if not raw:
        return "（无标题）"

    # Drop trailing "(#1234)" if still present.
    raw = re.sub(r"\s*\(#\d+\)\s*$", "", raw).strip()

    scope = ""
    msg = raw
    if ":" in raw:
        maybe_scope, maybe_msg = raw.split(":", 1)
        # Keep conservative: only treat it as scope when it looks like gem5 topic prefix.
        if 0 < len(maybe_scope.strip()) <= 40:
            scope = maybe_scope.strip()
            msg = maybe_msg.strip()

    msg_raw = msg  # keep raw EN title for pattern matching

    # Special-case common phrasing to make it more natural.
    m = re.match(r"^(make|makes)\s+(.+?)\s+optional\b(.*)$", msg_raw, flags=re.IGNORECASE)
    if m:
        inner_raw = m.group(2).strip()
        tail_raw = m.group(3).strip()
        # "using X when using Y" -> "在使用 Y 时使用 X"
        m2 = re.match(r"using\s+(.+?)\s+when\s+using\s+(.+)$", inner_raw, flags=re.IGNORECASE)
        if m2:
            inner_raw = f"在使用 {m2.group(2).strip()} 时使用 {m2.group(1).strip()}"
        inner = _rough_en_to_zh(inner_raw)
        tail = _rough_en_to_zh(tail_raw)
        core = f"使{inner} 变为可选"
        if tail:
            core += f" {tail}"
    else:
        # Verb-based templates.
        templates: list[tuple[str, str]] = [
            (r"^(fixes|fix)\s+(.+)$", "修复{obj}"),
            (r"^(adds|add)\s+support\s+for\s+(.+)$", "增加对{obj}的支持"),
            (r"^(adds|add)\s+(.+)$", "新增{obj}"),
            (r"^(removes|remove|drops|drop)\s+(.+)$", "移除{obj}"),
            (r"^(updates|update|bumps|bump|upgrades|upgrade)\s+(.+)$", "更新{obj}"),
            (r"^(refactor|rework|rewrite|cleanup|clean up)\s+(.+)$", "重构{obj}"),
            (r"^(improves|improve)\s+(.+)$", "改进{obj}"),
            (r"^(implements|implement)\s+(.+)$", "实现{obj}"),
            (r"^(enables|enable)\s+(.+)$", "启用{obj}"),
            (r"^(disables|disable)\s+(.+)$", "禁用{obj}"),
            (r"^(renames|rename)\s+(.+)$", "重命名{obj}"),
            (r"^(moves|move)\s+(.+)$", "迁移{obj}"),
            (r"^(changes|change)\s+(.+)$", "调整{obj}"),
        ]
        core = _rough_en_to_zh(msg_raw)
        for pat, tpl in templates:
            mm = re.match(pat, msg_raw, flags=re.IGNORECASE)
            if not mm:
                continue
            obj = mm.group(mm.lastindex or 1).strip()
            obj = _rough_en_to_zh(obj)
            # Fix common "X caused by Y" -> "Y 导致的 X"
            mcb = re.match(r"(.+?)\s+(caused by|due to)\s+(.+)$", obj, flags=re.IGNORECASE)
            if mcb:
                obj = f"{mcb.group(3).strip()} 导致的 {mcb.group(1).strip()}"
            # "X when Y" / "X if Y" -> "Y 时的 X"
            mwhen = re.match(r"(.+?)\s+when\s+(.+)$", obj, flags=re.IGNORECASE)
            if mwhen:
                obj = f"{mwhen.group(2).strip()} 时的 {mwhen.group(1).strip()}"
            mif = re.match(r"(.+?)\s+if\s+(.+)$", obj, flags=re.IGNORECASE)
            if mif:
                obj = f"{mif.group(2).strip()} 时的 {mif.group(1).strip()}"
            # "X in Y" -> "Y 中的 X"
            min_ = re.match(r"(.+?)\s+in\s+(.+)$", obj, flags=re.IGNORECASE)
            if min_:
                obj = f"{min_.group(2).strip()} 中的 {min_.group(1).strip()}"
            # Re-normalize after re-ordering.
            obj = _rough_en_to_zh(obj)
            core = tpl.format(obj=obj)
            break

    # Action label as a hint for scanning.
    prefix = action or "其他"
    if scope:
        return f"【{prefix}】{scope}：{core}。"
    return f"【{prefix}】{core}。"


def classify_action(subject: str) -> str:
    """
    Heuristic action classification based on commit subject.
    This is best-effort and intended to help scanning, not perfect labeling.
    """
    s = subject.lower()
    # Prefer more specific buckets first.
    rules: list[tuple[str, str]] = [
        (r"\b(doc|docs|readme|tutorial|learning-gem5)\b", "文档/示例"),
        (r"\b(test|tests|unittest|pyunit|regress)\b", "测试"),
        (r"\b(ci|github|workflow)\b", "CI"),
        (r"\b(bump|upgrade|update|pin|version)\b", "更新/依赖"),
        (r"\b(add|support|implement|introduce|enable|new)\b", "新增/支持"),
        (r"\b(refactor|rework|rewrite|cleanup|reorganize)\b", "重构/整理"),
        (r"\b(remove|delete|drop|deprecate)\b", "移除/弃用"),
        (r"\b(rename|move)\b", "重命名/迁移"),
        (r"\b(fix|bug|correct|avoid|prevent|handle|harden)\b", "修复/纠错"),
    ]
    for pat, label in rules:
        if re.search(pat, s):
            return label
    return "其他"


def arch_from_path(path: str) -> Optional[str]:
    # Keep this conservative and filesystem-based.
    # Typical gem5 layout: src/arch/<arch>/...
    if path.startswith("src/arch/"):
        parts = path.split("/", 3)
        if len(parts) >= 3 and parts[2]:
            return parts[2]
    return None


def subsys_from_path(path: str) -> str:
    # A lightweight subsystem bucketing for report readability.
    # Order matters: more specific first.
    p = path
    rules = [
        ("src/cpu/o3/", "cpu/o3"),
        ("src/cpu/minor/", "cpu/minor"),
        ("src/cpu/simple/", "cpu/simple"),
        ("src/cpu/pred/", "cpu/pred"),
        ("src/cpu/", "cpu"),
        ("src/mem/cache/prefetch/", "mem/cache/prefetch"),
        ("src/mem/cache/replacement_policies/", "mem/cache/rp"),
        ("src/mem/cache/", "mem/cache"),
        ("src/mem/", "mem"),
        ("src/dev/", "dev"),
        ("src/gpu-compute/", "gpu-compute"),
        ("src/arch/", "arch"),
        ("src/sim/", "sim"),
        ("src/base/", "base"),
        ("src/python/", "python"),
        ("configs/", "configs"),
        ("util/", "util"),
        ("ext/", "ext"),
        ("tests/", "tests"),
        ("docs/", "docs"),
        ("site_scons/", "scons"),
    ]
    for prefix, bucket in rules:
        if p.startswith(prefix):
            return bucket
    # Top-level fallback
    return p.split("/", 1)[0] if "/" in p else p


def parse_git_log_numstat(raw: str) -> tuple[list[CommitMeta], list[dict]]:
    """
    Parse `git log --numstat` output with a record separator prefix.

    Record layout:
      \x1e<commit>\x1f<parents>\x1f<author-date>\x1f<commit-date>\x1f<author>\x1f<email>\x1f<subject>\n
      <numstat lines...>
    """
    commits: list[CommitMeta] = []
    file_rows: list[dict] = []

    for rec in raw.split(RECORD_SEP):
        if not rec.strip():
            continue
        lines = rec.splitlines()
        if not lines:
            continue

        header = lines[0]
        parts = header.split(FIELD_SEP)
        if len(parts) != 7:
            # If something goes wrong, keep going but don't crash.
            continue

        commit, parents_s, author_date_iso, commit_date_iso, author_name, author_email, subject = parts
        parents = [p for p in parents_s.split() if p]
        meta = CommitMeta(
            commit=commit,
            parents=parents,
            author_name=author_name,
            author_email=author_email,
            # Use the committer date as the primary timestamp. Author dates can be
            # arbitrarily old when long-lived branches are merged.
            date_iso=commit_date_iso,
            subject=subject,
        )
        commits.append(meta)

        for ln in lines[1:]:
            if not ln.strip():
                continue
            cols = ln.split("\t")
            if len(cols) < 3:
                continue
            add_s, del_s, path = cols[0], cols[1], cols[2]
            add = int(add_s) if add_s.isdigit() else None
            dele = int(del_s) if del_s.isdigit() else None
            file_rows.append(
                {
                    "commit": commit,
                    "path": path,
                    "add": add,
                    "del": dele,
                    "arch": arch_from_path(path),
                    "subsys": subsys_from_path(path),
                    "topdir": path.split("/", 1)[0] if "/" in path else path,
                }
            )

    return commits, file_rows


def parse_git_log_body(raw: str) -> dict[str, str]:
    """
    Parse a RS/FS delimited log which encodes: <RS><commit><FS><body>\n
    Commit messages shouldn't contain NUL; RS/FS are extremely unlikely too.
    """
    out: dict[str, str] = {}
    for rec in raw.split(RECORD_SEP):
        if not rec.strip():
            continue
        if FIELD_SEP not in rec:
            continue
        commit, body = rec.split(FIELD_SEP, 1)
        out[commit.strip()] = body.rstrip("\n")
    return out


def body_excerpt(body: str, max_chars: int = 220) -> str:
    b = (body or "").strip()
    if not b:
        return ""
    # Take the first paragraph (until the first blank line), single-line.
    lines = [ln.strip() for ln in b.splitlines()]
    # Drop common boilerplate produced by Gerrit/CI tooling.
    boilerplate = re.compile(
        r"^(change-id|reviewed-on|reviewed-by|tested-by|merged-by|co-authored-by|"
        r"signed-off-by|bug|issue|jira|task):\s*",
        re.IGNORECASE,
    )
    lines = [ln for ln in lines if ln and not boilerplate.match(ln)]
    while lines and not lines[0]:
        lines.pop(0)
    para: list[str] = []
    for ln in lines:
        if not ln:
            break
        para.append(ln)
    s = " ".join(para).strip()
    if len(s) > max_chars:
        return s[: max_chars - 1].rstrip() + "…"
    return s


def extract_release_highlights(release_notes_md: str) -> dict[str, list[str]]:
    """
    Extract '## Major Highlights' bullet titles for each '# Version X' section.
    Returns: version -> [highlight title, ...]
    """
    lines = release_notes_md.splitlines()
    # Locate version sections.
    version_starts: list[tuple[str, int]] = []
    for i, ln in enumerate(lines):
        m = re.match(r"^# Version ([0-9][0-9.]+)\s*$", ln)
        if m:
            version_starts.append((m.group(1), i))
    version_starts.append(("_EOF_", len(lines)))

    out: dict[str, list[str]] = {}
    for (ver, start), (_, end) in zip(version_starts[:-1], version_starts[1:]):
        section = lines[start:end]
        # Find Major Highlights subsection.
        try:
            mh_idx = next(i for i, ln in enumerate(section) if ln.strip() == "## Major Highlights")
        except StopIteration:
            continue
        bullets: list[str] = []
        for ln in section[mh_idx + 1 :]:
            if ln.startswith("## "):
                break
            # Example line:
            #   * **Neoverse V2 core model.**
            # Match both:
            #   * **Title.**
            #   - **Title**: description...
            m = re.match(r"^[*-] \*\*(.+?)\*\*", ln)
            if m:
                bullets.append(m.group(1).strip())
        if bullets:
            out[ver] = bullets
    return out


def extract_release_sections(release_notes_md: str) -> dict[str, list[str]]:
    """
    Split RELEASE-NOTES.md into sections keyed by version string (without the leading 'v').
    Example key: "25.1", "25.0.0.1".
    """
    lines = release_notes_md.splitlines()
    version_starts: list[tuple[str, int]] = []
    for i, ln in enumerate(lines):
        m = re.match(r"^# Version ([0-9][0-9.]+)\s*$", ln)
        if m:
            version_starts.append((m.group(1), i))
    version_starts.append(("_EOF_", len(lines)))

    out: dict[str, list[str]] = {}
    for (ver, start), (_, end) in zip(version_starts[:-1], version_starts[1:]):
        out[ver] = lines[start:end]
    return out


def summarize_release_section_zh(lines: list[str], max_bullets: int = 12) -> list[str]:
    """
    A lightweight Chinese digest from a release note section.
    We do not attempt full translation; we extract prominent bullet points and
    add a Chinese action prefix to improve readability.
    """

    def zh_prefix(ln: str) -> str:
        t = ln.strip()
        low = t.lower()
        verbs = [
            ("fix", "修复："),
            ("add", "新增："),
            ("support", "支持："),
            ("update", "更新："),
            ("bump", "升级："),
            ("remove", "移除："),
            ("deprecate", "弃用："),
            ("refactor", "重构："),
            ("rework", "重构："),
            ("improve", "改进："),
            ("change", "变更："),
        ]
        for v, p in verbs:
            if low.startswith(v + " "):
                return p
        return "要点："

    bullets: list[str] = []
    in_code_block = False
    # Collect:
    #  - Level-3 headings ("### X") as feature anchors.
    #  - Top-level bullets ("* " or "- ") with no indentation.
    for ln in lines:
        if ln.startswith("```"):
            in_code_block = not in_code_block
            continue
        if in_code_block:
            continue
        if ln.startswith("# Version "):
            continue
        if ln.startswith("### "):
            title = ln[4:].strip()
            if title:
                bullets.append(f"主题：{_rough_en_to_zh(title)}")
                if len(bullets) >= max_bullets:
                    break
        if ln.startswith("## "):
            # Stop after we pass the first couple of big subsections to keep it short.
            # We still continue; this just allows later bullets to be picked too.
            pass
        m = re.match(r"^[*-] (.+)$", ln)
        if not m:
            continue
        content = m.group(1).strip()
        if not content:
            continue
        # Skip link-only bullets.
        if re.match(r"^\\[?#\\d+\\]?$", content):
            continue
        bullets.append(f"{zh_prefix(content)}{_rough_en_to_zh(content)}")
        if len(bullets) >= max_bullets:
            break
    return bullets


def format_set(items: Iterable[str], limit: int = 4) -> str:
    items = [x for x in items if x]
    if not items:
        return "-"
    items_sorted = sorted(set(items))
    if len(items_sorted) <= limit:
        return ", ".join(items_sorted)
    return ", ".join(items_sorted[:limit]) + f", +{len(items_sorted) - limit} more"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=".", type=str, help="Path to git repo")
    ap.add_argument("--origin-ref", default="origin/xs-dev", help="OpenXiangShan default branch ref")
    ap.add_argument("--upstream-ref", default="upstream/stable", help="gem5 upstream ref to summarize")
    ap.add_argument("--base", default="", help="Fork/base commit. If empty, computed via merge-base(origin, upstream)")
    ap.add_argument("--outdir", default=".codex/reports/upstream_since_fork_a3be84c", help="Output directory")
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    outdir = Path(args.outdir).resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "data").mkdir(exist_ok=True)
    (outdir / "figures").mkdir(exist_ok=True)

    origin_ref = args.origin_ref
    upstream_ref = args.upstream_ref

    base = args.base.strip() or git_merge_base(repo, origin_ref, upstream_ref)
    base_tag = git_describe_tag(repo, base)
    upstream_nearest_tag = git_describe_tag(repo, upstream_ref)
    upstream_head = git_rev_parse(repo, upstream_ref)
    origin_head = git_rev_parse(repo, origin_ref)

    # Extract commit metadata + numstat in bulk.
    # Use topo-order to avoid pulling very old commits (by commit date) to the front.
    # Include both author and committer dates; we use committer date for reporting.
    fmt = (
        f"{RECORD_SEP}%H{FIELD_SEP}%P{FIELD_SEP}%ad{FIELD_SEP}%cd"
        f"{FIELD_SEP}%aN{FIELD_SEP}%aE{FIELD_SEP}%s%n"
    )
    range_spec = f"{base}..{upstream_ref}"
    raw = run_git(
        repo,
        [
            "log",
            "--reverse",
            "--topo-order",
            "--date=iso-strict",
            f"--format={fmt}",
            "--numstat",
            range_spec,
        ],
    )
    commits, file_rows = parse_git_log_numstat(raw)

    # Build dataframes.
    commits_df = pd.DataFrame(
        [
            {
                "commit": c.commit,
                "short": c.short,
                "parents": " ".join(c.parents),
                "is_merge": c.is_merge,
                "date": c.date_iso,
                "ymd": c.ymd,
                "ym": c.ym,
                "author": c.author_name,
                "email": c.author_email,
                "subject": c.subject,
                "topic": topic_prefix(c.subject),
            }
            for c in commits
        ]
    )
    files_df = pd.DataFrame(file_rows)

    # Aggregate per-commit stats.
    if not files_df.empty:
        per_commit = (
            files_df.groupby("commit")
            .agg(
                files=("path", "count"),
                insertions=("add", lambda s: int(pd.Series([x for x in s if pd.notna(x)]).sum()) if len(s) else 0),
                deletions=("del", lambda s: int(pd.Series([x for x in s if pd.notna(x)]).sum()) if len(s) else 0),
            )
            .reset_index()
        )
        commits_df = commits_df.merge(per_commit, on="commit", how="left")
    else:
        commits_df["files"] = 0
        commits_df["insertions"] = 0
        commits_df["deletions"] = 0

    commits_df[["files", "insertions", "deletions"]] = (
        commits_df[["files", "insertions", "deletions"]].fillna(0).astype(int)
    )

    # Symmetric diff counts (origin-only vs upstream-only).
    left_right = run_git(repo, ["rev-list", "--left-right", "--count", f"{origin_ref}...{upstream_ref}"]).strip()
    origin_only, upstream_only = (0, 0)
    try:
        origin_only, upstream_only = (int(x) for x in left_right.split())
    except Exception:
        pass

    # Save datasets.
    commits_csv = outdir / "data" / "commits.csv"
    files_csv = outdir / "data" / "files.csv"
    commits_df.to_csv(commits_csv, index=False)
    if not files_df.empty:
        files_df.to_csv(files_csv, index=False)
    else:
        # Still create an empty file for tooling stability.
        with open(files_csv, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["commit", "path", "add", "del", "arch", "subsys", "topdir"])

    # Overview stats.
    total_commits = len(commits_df)
    merge_commits = int(commits_df["is_merge"].sum()) if total_commits else 0
    first_date = commits_df["date"].iloc[0] if total_commits else ""
    last_date = commits_df["date"].iloc[-1] if total_commits else ""
    unique_authors = int(commits_df["author"].nunique()) if total_commits else 0

    # Top buckets for textual summary.
    top_topics = commits_df["topic"].value_counts().head(15)
    subsys_top = pd.Series(dtype=int)
    arch_top = pd.Series(dtype=int)
    topdirs_top = pd.Series(dtype=int)
    if not files_df.empty:
        subsys_top = files_df.groupby("subsys")["commit"].nunique().sort_values(ascending=False).head(15)
        arch_top = (
            files_df.dropna(subset=["arch"])
            .groupby("arch")["commit"]
            .nunique()
            .sort_values(ascending=False)
            .head(15)
        )
        topdirs_top = files_df.groupby("topdir")["commit"].nunique().sort_values(ascending=False).head(15)

    # Visualization: commits per month.
    fig_dir = outdir / "figures"
    if total_commits:
        by_month = commits_df.groupby("ym").size().sort_index()
        plt.figure(figsize=(14, 4))
        by_month.plot(kind="bar")
        plt.title(f"Upstream commits per month ({upstream_ref})")
        plt.xlabel("YYYY-MM")
        plt.ylabel("commits")
        plt.tight_layout()
        plt.savefig(fig_dir / "commits_per_month.png", dpi=200)
        plt.close()

    # Visualization: top topics.
    if total_commits:
        by_topic = commits_df["topic"].value_counts().head(20)[::-1]
        plt.figure(figsize=(10, 6))
        by_topic.plot(kind="barh")
        plt.title("Top 20 commit subject prefixes (topic)")
        plt.xlabel("commits")
        plt.tight_layout()
        plt.savefig(fig_dir / "top_topics.png", dpi=200)
        plt.close()

    # Visualization: arch activity (commit count touching src/arch/<arch>/).
    if not files_df.empty:
        arch_commits = (
            files_df.dropna(subset=["arch"])
            .groupby("arch")["commit"]
            .nunique()
            .sort_values(ascending=False)
            .head(20)
        )
        if not arch_commits.empty:
            plt.figure(figsize=(10, 6))
            arch_commits[::-1].plot(kind="barh")
            plt.title("Top arch directories by unique commits (src/arch/<arch>)")
            plt.xlabel("unique commits")
            plt.tight_layout()
            plt.savefig(fig_dir / "arch_activity.png", dpi=200)
            plt.close()

        topdirs = files_df.groupby("topdir")["commit"].nunique().sort_values(ascending=False).head(20)
        plt.figure(figsize=(10, 6))
        topdirs[::-1].plot(kind="barh")
        plt.title("Top-level dirs by unique commits")
        plt.xlabel("unique commits")
        plt.tight_layout()
        plt.savefig(fig_dir / "topdirs_activity.png", dpi=200)
        plt.close()

    # Release notes highlights (from upstream ref).
    rel_notes = ""
    try:
        rel_notes = run_git(repo, ["show", f"{upstream_ref}:RELEASE-NOTES.md"])
    except subprocess.CalledProcessError:
        rel_notes = ""
    highlights = extract_release_highlights(rel_notes) if rel_notes else {}
    release_sections = extract_release_sections(rel_notes) if rel_notes else {}

    # Extract commit bodies in bulk for richer per-commit summaries.
    raw_body = run_git(
        repo,
        [
            "log",
            "--reverse",
            "--topo-order",
            "--format=" + f"{RECORD_SEP}%H{FIELD_SEP}%b%n",
            f"{base}..{upstream_ref}",
        ],
    )
    commit_body = parse_git_log_body(raw_body)

    # Enrich commit dataframe with PR number and a cleaner "title" for display.
    pr_list: list[Optional[int]] = []
    title_list: list[str] = []
    action_list: list[str] = []
    for row in commits_df.itertuples(index=False):
        body = commit_body.get(row.commit, "")
        pr = extract_pr_number_any(row.subject, body)
        title = clean_pr_title(row.subject, pr, body)
        pr_list.append(pr)
        title_list.append(title)
        action_list.append(classify_action(title))
    commits_df["pr"] = pd.array(pr_list, dtype="Int64")
    commits_df["title"] = title_list
    commits_df["action"] = action_list

    # Release tag milestones between base tag and upstream nearest tag (if available).
    base_ver = parse_version_tuple(base_tag) if base_tag else None
    upstream_ver = parse_version_tuple(upstream_nearest_tag) if upstream_nearest_tag else None
    milestones: list[dict] = []
    try:
        merged_tags = run_git(repo, ["tag", "--merged", upstream_ref]).splitlines()
    except subprocess.CalledProcessError:
        merged_tags = []
    cand_tags: list[str] = []
    for t in merged_tags:
        v = parse_version_tuple(t.strip())
        if not v:
            continue
        if base_ver and v < base_ver:
            continue
        if upstream_ver and v > upstream_ver:
            continue
        cand_tags.append(t.strip())
    cand_tags = sorted(set(cand_tags), key=lambda t: parse_version_tuple(t) or (0,))
    # Keep it readable: only versioned tags in this range are already small (< ~30).
    for t in cand_tags:
        try:
            # Peel annotated tags to the commit they reference.
            commit = run_git(repo, ["rev-parse", f"{t}^{{}}"]).strip()
            info = run_git(repo, ["show", "-s", "--date=short", "--format=%ad%x1f%s", commit]).strip()
            d, s = info.split(FIELD_SEP)
            milestones.append(
                {
                    "tag": t,
                    "date": d,
                    "commit": commit,
                    "short": commit[:12],
                    "subject": s,
                }
            )
        except Exception:
            continue

    # Build README (overview).
    readme = outdir / "README.md"
    with open(readme, "w", encoding="utf-8") as f:
        f.write("# Upstream Changes Since Fork Point\n\n")
        f.write("## Baseline\n\n")
        f.write(f"- origin ref: `{origin_ref}` = `{origin_head}`\n")
        f.write(f"- upstream ref: `{upstream_ref}` = `{upstream_head}`\n")
        f.write(f"- fork/base (merge-base): `{base}`\n")
        if base_tag:
            f.write(f"- base tag: `{base_tag}`\n")
        if upstream_nearest_tag:
            f.write(f"- upstream nearest tag: `{upstream_nearest_tag}`\n")
        f.write("\n")
        f.write("## Stats\n\n")
        f.write(f"- commits in range `{base}..{upstream_ref}`: **{total_commits}** (merge commits: {merge_commits})\n")
        if first_date and last_date:
            f.write(f"- time span: `{first_date}` -> `{last_date}`\n")
        f.write(f"- unique authors: {unique_authors}\n")
        f.write(f"- symmetric diff vs `{origin_ref}`: origin-only={origin_only}, upstream-only={upstream_only}\n")
        f.write("\n")
        if milestones:
            f.write("## Release tags in range\n\n")
            f.write("| tag | date | commit | subject |\n")
            f.write("|---|---|---|---|\n")
            for m in milestones:
                f.write(f"| `{m['tag']}` | {m['date']} | `{m['short']}` | {m['subject']} |\n")
            f.write("\n")

        if not top_topics.empty:
            f.write("## Top topics (commit subject prefixes)\n\n")
            f.write("| topic | commits |\n|---|---:|\n")
            for k, v in top_topics.items():
                f.write(f"| `{k}` | {int(v)} |\n")
            f.write("\n")
        if not topdirs_top.empty:
            f.write("## Top directories (unique commits)\n\n")
            f.write("| dir | unique commits |\n|---|---:|\n")
            for k, v in topdirs_top.items():
                f.write(f"| `{k}` | {int(v)} |\n")
            f.write("\n")
        if not subsys_top.empty:
            f.write("## Top subsystems (unique commits)\n\n")
            f.write("| subsystem | unique commits |\n|---|---:|\n")
            for k, v in subsys_top.items():
                f.write(f"| `{k}` | {int(v)} |\n")
            f.write("\n")
        if not arch_top.empty:
            f.write("## Top arch directories (unique commits under src/arch/<arch>)\n\n")
            f.write("| arch | unique commits |\n|---|---:|\n")
            for k, v in arch_top.items():
                f.write(f"| `{k}` | {int(v)} |\n")
            f.write("\n")

        f.write("## Visualizations\n\n")
        if (fig_dir / "commits_per_month.png").exists():
            f.write("![commits per month](figures/commits_per_month.png)\n\n")
        # This plot is generated later in the script; include it unconditionally.
        f.write("![prs per month](figures/prs_per_month.png)\n\n")
        if (fig_dir / "top_topics.png").exists():
            f.write("![top topics](figures/top_topics.png)\n\n")
        if (fig_dir / "arch_activity.png").exists():
            f.write("![arch activity](figures/arch_activity.png)\n\n")
        if (fig_dir / "topdirs_activity.png").exists():
            f.write("![top dirs activity](figures/topdirs_activity.png)\n\n")

        f.write("## Release Major Highlights (from RELEASE-NOTES.md)\n\n")
        if highlights:
            # Focus on versions >= base major and <= upstream tag major. Keep only >=22.0.
            def ver_key(v: str) -> tuple[int, ...]:
                return tuple(int(x) for x in v.split("."))

            for ver in sorted(highlights.keys(), key=ver_key, reverse=True):
                if ver_key(ver) < (22, 0):
                    continue
                f.write(f"### Version {ver}\n\n")
                for h in highlights[ver]:
                    f.write(f"- {h}\n")
                f.write("\n")
        else:
            f.write("- (RELEASE-NOTES.md not found on this ref)\n\n")

        f.write("## Detailed per-commit list\n\n")
        f.write("- `commits.md`: one-line explanation per commit (chronological)\n")
        f.write("- `commits_detailed_zh.md`: per-commit detailed digest (Chinese)\n")
        f.write("- `prs_by_release_zh.md`: PR summary grouped by release tag (Chinese)\n")
        f.write("- `prs_one_liner_by_release_zh.md`: PR one-liners grouped by release tag (Chinese)\n")
        f.write("- `prs_detailed_zh.md`: per-PR digest (Chinese)\n")
        f.write("- `releases_zh.md`: release/tag digest (Chinese)\n")
        f.write("- `focus_subsystems_zh.md`: directory/subsystem-focused digest (Chinese)\n")
        f.write("- `overview_zh.md`: human-friendly entry (Chinese)\n")
        f.write("- `data/commits.csv`: commit metadata + aggregated numstat\n")
        f.write("- `data/files.csv`: per-file numstat (may be large)\n")
        f.write("- `data/prs.csv`: per-PR aggregation\n")
        f.write("\n")
        f.write("## Reproduce\n\n")
        f.write("```bash\n")
        f.write(f"python3 {readme.parent / 'scripts' / 'generate_upstream_report.py'} \\\n")
        f.write(f"  --origin-ref {origin_ref} --upstream-ref {upstream_ref} --base {base} \\\n")
        f.write(f"  --outdir {outdir}\n")
        f.write("```\n")

    # Build a Chinese entry doc as the main deliverable for local usage.
    readme_zh = outdir / "README_zh.md"
    with open(readme_zh, "w", encoding="utf-8") as f:
        f.write("# upstream 变更总结（相对 OpenXiangShan/GEM5 分叉点）\n\n")
        f.write("## 摘要\n\n")
        f.write(
            f"- 分叉基线：`{base}`"
            + (f"（tag: `{base_tag}`）" if base_tag else "")
            + "\n"
        )
        f.write(f"- upstream 参考：`{upstream_ref}` = `{upstream_head}`\n")
        if upstream_nearest_tag:
            f.write(f"- upstream 最近 tag：`{upstream_nearest_tag}`\n")
        f.write(f"- upstream 相对基线新增 commit：**{total_commits}**（merge commits: {merge_commits}）\n")
        f.write(f"- 与 `{origin_ref}` 的对比：origin-only={origin_only}，upstream-only={upstream_only}\n\n")

        f.write("## 复现步骤（可重复）\n\n")
        f.write("```bash\n")
        f.write("git fetch --all --prune\n")
        f.write(f"git merge-base {origin_ref} {upstream_ref}\n")
        f.write(f"git rev-list --left-right --count {origin_ref}...{upstream_ref}\n")
        f.write(f"python3 {readme.parent / 'scripts' / 'generate_upstream_report.py'} \\\n")
        f.write(f"  --origin-ref {origin_ref} --upstream-ref {upstream_ref} --base {base} \\\n")
        f.write(f"  --outdir {outdir}\n")
        f.write("```\n\n")

        f.write("## 统计概览\n\n")
        if first_date and last_date:
            f.write(f"- 时间跨度（commit date）：`{first_date}` -> `{last_date}`\n")
        f.write(f"- 贡献者（author 去重）：{unique_authors}\n\n")

        if milestones:
            f.write("## Release 里程碑（tag）\n\n")
            f.write("| tag | date | commit | subject |\n")
            f.write("|---|---|---|---|\n")
            for m in milestones:
                f.write(f"| `{m['tag']}` | {m['date']} | `{m['short']}` | {m['subject']} |\n")
            f.write("\n")

        f.write("## 可视化\n\n")
        if (fig_dir / "commits_per_month.png").exists():
            f.write("![每月 commit 数](figures/commits_per_month.png)\n\n")
        # This plot is generated later in the script; include it unconditionally.
        f.write("![每月 PR 数](figures/prs_per_month.png)\n\n")
        if (fig_dir / "top_topics.png").exists():
            f.write("![Top topic 前缀](figures/top_topics.png)\n\n")
        if (fig_dir / "arch_activity.png").exists():
            f.write("![Arch 活跃度](figures/arch_activity.png)\n\n")
        if (fig_dir / "topdirs_activity.png").exists():
            f.write("![Top-level 目录活跃度](figures/topdirs_activity.png)\n\n")

        if not top_topics.empty:
            f.write("## 主要改动主题（commit message 前缀 Top 15）\n\n")
            f.write("| topic | commits |\n|---|---:|\n")
            for k, v in top_topics.items():
                f.write(f"| `{k}` | {int(v)} |\n")
            f.write("\n")
        if not subsys_top.empty:
            f.write("## 主要改动子系统（unique commits Top 15）\n\n")
            f.write("| subsystem | unique commits |\n|---|---:|\n")
            for k, v in subsys_top.items():
                f.write(f"| `{k}` | {int(v)} |\n")
            f.write("\n")
        if not arch_top.empty:
            f.write("## 主要改动架构（src/arch/<arch> unique commits Top 15）\n\n")
            f.write("| arch | unique commits |\n|---|---:|\n")
            for k, v in arch_top.items():
                f.write(f"| `{k}` | {int(v)} |\n")
            f.write("\n")

        f.write("## 上游 release highlights（摘自 RELEASE-NOTES.md）\n\n")
        if highlights:
            def ver_key(v: str) -> tuple[int, ...]:
                return tuple(int(x) for x in v.split("."))

            for ver in sorted(highlights.keys(), key=ver_key, reverse=True):
                if base_ver and ver_key(ver) < base_ver:
                    continue
                if upstream_ver and ver_key(ver) > upstream_ver:
                    continue
                f.write(f"### Version {ver}\n\n")
                for h in highlights[ver]:
                    f.write(f"- {h}\n")
                f.write("\n")
        else:
            f.write("- （此 ref 上未解析到 Major Highlights；可直接查看 `RELEASE-NOTES.md` 原文）\n\n")

        f.write("## 逐 commit 细节\n\n")
        f.write("- 每个 commit 一行说明：`commits.md`\n")
        f.write("- 每个 commit 详细摘要：`commits_detailed_zh.md`\n")
        f.write("- PR 汇总（按 release tag）：`prs_by_release_zh.md`\n")
        f.write("- PR 一句话摘要（按 release tag）：`prs_one_liner_by_release_zh.md`\n")
        f.write("- PR 逐条摘要：`prs_detailed_zh.md`\n")
        f.write("- release 版本摘要（按 tag/RELEASE-NOTES）：`releases_zh.md`\n")
        f.write("- 重点目录聚合：`focus_subsystems_zh.md`\n")
        f.write("- 人类友好入口：`overview_zh.md`\n")
        f.write("- 原始数据：`data/commits.csv`（commit 元信息 + 聚合 numstat）\n")
        f.write("- 原始数据：`data/files.csv`（逐文件 numstat，体积较大）\n")
        f.write("- 原始数据：`data/prs.csv`（PR 聚合数据）\n")

    # Build per-commit markdown (dense, one-line each).
    commits_md = outdir / "commits.md"
    # Precompute per-commit sets for arch/subsys/topdir to show in one-line bullets.
    commit_to_arch: dict[str, set[str]] = defaultdict(set)
    commit_to_subsys: dict[str, set[str]] = defaultdict(set)
    commit_to_topdir: dict[str, set[str]] = defaultdict(set)
    if not files_df.empty:
        for row in files_df.itertuples(index=False):
            commit_to_topdir[row.commit].add(row.topdir)
            if row.arch:
                commit_to_arch[row.commit].add(row.arch)
            if row.subsys:
                commit_to_subsys[row.commit].add(row.subsys)

    with open(commits_md, "w", encoding="utf-8") as f:
        f.write(f"# Upstream commits: `{base}..{upstream_ref}` (chronological)\n\n")
        f.write(f"- total: {total_commits} (merges: {merge_commits})\n")
        f.write(f"- base: `{base}` ({base_tag or 'no-tag'})\n")
        f.write(f"- upstream: `{upstream_ref}` ({upstream_head})\n\n")

        # Keep the git log order (topo-order). Insert a heading when the commit month changes.
        last_ym = None
        for row in commits_df.itertuples(index=False):
            if row.ym != last_ym:
                f.write(f"## {row.ym}\n\n")
                last_ym = row.ym
            archs = format_set(commit_to_arch.get(row.commit, set()))
            subsys = format_set(commit_to_subsys.get(row.commit, set()))
            topdirs = format_set(commit_to_topdir.get(row.commit, set()))
            merge_mark = " MERGE" if row.is_merge else ""
            f.write(
                f"- {row.ymd} `{row.short}`{merge_mark} {row.title} "
                f"(files:{row.files}, +{row.insertions}/-{row.deletions}; "
                f"top:{topdirs}; subsys:{subsys}; arch:{archs})\n"
            )
        f.write("\n")

    # Build a richer per-commit digest (Chinese), still compact enough to grep.
    commits_detailed_zh = outdir / "commits_detailed_zh.md"

    commit_to_file_entries: dict[str, list[tuple[int, str, Optional[int], Optional[int]]]] = defaultdict(list)
    for r in file_rows:
        add = r["add"]
        dele = r["del"]
        churn = (add or 0) + (dele or 0) if (add is not None and dele is not None) else 0
        commit_to_file_entries[r["commit"]].append((churn, r["path"], add, dele))

    def fmt_add_del(add: Optional[int], dele: Optional[int]) -> str:
        if add is None or dele is None:
            return "bin/rename"
        return f"+{add}/-{dele}"

    with open(commits_detailed_zh, "w", encoding="utf-8") as f:
        f.write(f"# upstream 逐 commit 详细摘要：`{base}..{upstream_ref}`\n\n")
        f.write("- 说明：该文件为“机器生成”的 digest，便于快速浏览/检索；要看完整改动请用 `git show <hash>`。\n")
        f.write(f"- commits: {total_commits} (merges: {merge_commits})\n")
        f.write(f"- base: `{base}` ({base_tag or 'no-tag'})\n")
        f.write(f"- upstream: `{upstream_ref}` ({upstream_head})\n\n")

        last_ym = None
        for row in commits_df.itertuples(index=False):
            if row.ym != last_ym:
                f.write(f"## {row.ym}\n\n")
                last_ym = row.ym

            merge_mark = "（MERGE）" if row.is_merge else ""
            pr = None if pd.isna(row.pr) else int(row.pr)
            action = row.action
            archs = format_set(commit_to_arch.get(row.commit, set()))
            subsys = format_set(commit_to_subsys.get(row.commit, set()))
            topdirs = format_set(commit_to_topdir.get(row.commit, set()))

            f.write(f"### {row.ymd} `{row.short}` {merge_mark} {row.title}\n\n")
            if row.title != row.subject:
                f.write(f"- 原始 subject: {row.subject}\n")
            f.write(f"- 动作（heuristic）: {action}\n")
            if pr is not None:
                f.write(f"- PR: #{pr} (https://github.com/gem5/gem5/pull/{pr})\n")
            f.write(f"- 影响范围: top={topdirs}; subsys={subsys}; arch={archs}\n")
            f.write(f"- 变更规模: files={row.files}, +{row.insertions}/-{row.deletions}\n")

            excerpt = body_excerpt(commit_body.get(row.commit, ""))
            if excerpt:
                f.write(f"- 备注（commit message 摘要）: {excerpt}\n")

            entries = commit_to_file_entries.get(row.commit, [])
            if entries:
                entries_sorted = sorted(entries, key=lambda x: (x[0], x[1]), reverse=True)
                top_n = 8
                show = entries_sorted[:top_n]
                f.write(f"- 主要改动文件（Top {top_n} by churn）:\n")
                for churn, path, add, dele in show:
                    f.write(f"  - `{path}` ({fmt_add_del(add, dele)})\n")
                if len(entries_sorted) > top_n:
                    f.write(f"  - ... +{len(entries_sorted) - top_n} files\n")
            f.write(f"- 复现: `git show {row.commit}`\n\n")

    # ==========================
    # PR-level aggregation
    # ==========================

    commit_title: dict[str, str] = {r.commit: r.title for r in commits_df.itertuples(index=False)}
    commit_ymd: dict[str, str] = {r.commit: r.ymd for r in commits_df.itertuples(index=False)}
    commit_date: dict[str, str] = {r.commit: r.date for r in commits_df.itertuples(index=False)}
    commit_churn: dict[str, int] = {
        r.commit: int(r.insertions) + int(r.deletions) for r in commits_df.itertuples(index=False)
    }
    commit_pr: dict[str, Optional[int]] = {
        r.commit: (None if pd.isna(r.pr) else int(r.pr)) for r in commits_df.itertuples(index=False)
    }

    # Canonicalize release tags (many tags point to the same commit, e.g., v25.1 and v25.1.0.0).
    best_tag_by_commit: dict[str, dict] = {}
    for m in milestones:
        c = m["commit"]
        v = parse_version_tuple(m["tag"])
        if not v:
            continue
        cur = best_tag_by_commit.get(c)
        if cur is None:
            best_tag_by_commit[c] = m
            continue
        cur_v = parse_version_tuple(cur["tag"]) or (0,)
        if v > cur_v:
            best_tag_by_commit[c] = m

    # Build a release timeline (dedup by commit) and compute per-release commit sets using git.
    release_points: list[dict] = sorted(
        best_tag_by_commit.values(),
        key=lambda x: (x["date"], parse_version_tuple(x["tag"]) or (0,)),
    )

    # For release breakdown, we bucket *upstream-only commits* (i.e., `base..upstream_ref`)
    # into release-tag intervals: (prev_tag..tag]. This works even if the fork-point
    # commit is on a parallel branch and not contained in early tags.
    upstream_only_set: set[str] = set(commits_df["commit"].tolist())

    release_info: dict[str, dict] = {}
    commit_to_release: dict[str, str] = {}
    prev_tag_commit: Optional[str] = None
    for rp in release_points:
        tag = rp["tag"]
        c = rp["commit"]
        if prev_tag_commit is None:
            # Baseline tag; no interval.
            release_info[tag] = {**rp, "commits_set": set(), "commits": 0, "churn": 0, "baseline": True}
            prev_tag_commit = c
            continue

        interval_all = set(run_git(repo, ["rev-list", f"{prev_tag_commit}..{c}"]).splitlines())
        commits_set = interval_all.intersection(upstream_only_set)
        churn = sum(commit_churn.get(h, 0) for h in commits_set)
        release_info[tag] = {
            **rp,
            "commits_set": commits_set,
            "commits": len(commits_set),
            "churn": churn,
            "baseline": False,
        }
        for h in commits_set:
            commit_to_release[h] = tag
        prev_tag_commit = c

    pr_to_commits: dict[int, list[str]] = defaultdict(list)
    pr_primary: dict[int, tuple[int, int, str]] = {}  # pr -> (weight, order, commit)

    def pr_subject_weight(subject: str, pr: int) -> int:
        if re.search(rf"\s*\(#\s*{pr}\s*\)\s*$", subject):
            return 2
        if re.search(rf"Merge pull request #\s*{pr}\b", subject, re.IGNORECASE):
            return 1
        if re.search(rf"github\.com/gem5/gem5/pull/{pr}\b", subject):
            return 1
        return 0

    for order, row in enumerate(commits_df.itertuples(index=False)):
        pr = None if pd.isna(row.pr) else int(row.pr)
        if pr is None:
            continue
        pr_to_commits[pr].append(row.commit)
        w = pr_subject_weight(row.subject, pr)
        cur = pr_primary.get(pr)
        if cur is None or (w, order) > (cur[0], cur[1]):
            pr_primary[pr] = (w, order, row.commit)

    # Aggregate file-level stats per PR.
    pr_files: dict[int, set[str]] = defaultdict(set)
    pr_insertions: dict[int, int] = defaultdict(int)
    pr_deletions: dict[int, int] = defaultdict(int)
    pr_topdirs: dict[int, Counter] = defaultdict(Counter)
    pr_subsys: dict[int, Counter] = defaultdict(Counter)
    pr_arch: dict[int, Counter] = defaultdict(Counter)
    pr_file_churn: dict[int, Counter] = defaultdict(Counter)
    for r in file_rows:
        pr = commit_pr.get(r["commit"])
        if pr is None:
            continue
        pr_files[pr].add(r["path"])
        pr_topdirs[pr][r["topdir"]] += 1
        pr_subsys[pr][r["subsys"]] += 1
        if r["arch"]:
            pr_arch[pr][r["arch"]] += 1
        add = r["add"]
        dele = r["del"]
        if add is not None:
            pr_insertions[pr] += int(add)
        if dele is not None:
            pr_deletions[pr] += int(dele)
        if add is not None and dele is not None:
            pr_file_churn[pr][r["path"]] += int(add) + int(dele)

    # Build PR dataset.
    prs_rows: list[dict] = []
    commits_with_pr = int((~commits_df["pr"].isna()).sum()) if total_commits else 0
    commits_without_pr = int(commits_df["pr"].isna().sum()) if total_commits else 0
    for pr, commits in pr_to_commits.items():
        _, rep_order, rep_commit = pr_primary[pr]
        title = commit_title.get(rep_commit, "")
        action = classify_action(title)
        dates = [commit_date.get(c, "") for c in commits if commit_date.get(c, "")]
        start_date = min(dates) if dates else ""
        end_date = max(dates) if dates else ""
        release_tag = commit_to_release.get(rep_commit) or (upstream_nearest_tag or upstream_ref)
        archs = sorted(pr_arch[pr].keys())
        subsys_top = [k for k, _ in pr_subsys[pr].most_common(6)]
        topdirs_top = [k for k, _ in pr_topdirs[pr].most_common(6)]
        prs_rows.append(
            {
                "pr": pr,
                "title": title,
                "action": action,
                "release_tag": release_tag,
                "rep_commit": rep_commit,
                "rep_ymd": commit_ymd.get(rep_commit, ""),
                "rep_order": rep_order,
                "commits": len(commits),
                "files": len(pr_files[pr]),
                "insertions": pr_insertions[pr],
                "deletions": pr_deletions[pr],
                "churn": pr_insertions[pr] + pr_deletions[pr],
                "start_date": start_date,
                "end_date": end_date,
                "topdirs": ", ".join(topdirs_top),
                "subsys": ", ".join(subsys_top),
                "arch": ", ".join(archs) if archs else "",
            }
        )

    prs_df = (
        pd.DataFrame(prs_rows).sort_values(["rep_ymd", "rep_order", "pr"])
        if prs_rows
        else pd.DataFrame()
    )
    prs_csv = outdir / "data" / "prs.csv"
    prs_df.to_csv(prs_csv, index=False)

    # Visualization: PRs per month (by representative commit date).
    if not prs_df.empty:
        prs_df["ym"] = prs_df["rep_ymd"].astype(str).str.slice(0, 7)
        prs_per_month = prs_df.groupby("ym").size().sort_index()
        plt.figure(figsize=(14, 4))
        prs_per_month.plot(kind="bar")
        plt.title(f"Upstream PRs per month ({upstream_ref})")
        plt.xlabel("YYYY-MM")
        plt.ylabel("PRs")
        plt.tight_layout()
        plt.savefig(fig_dir / "prs_per_month.png", dpi=200)
        plt.close()

    # ==========================
    # PR reports (Chinese)
    # ==========================

    prs_detailed_zh = outdir / "prs_detailed_zh.md"
    prs_by_release_zh = outdir / "prs_by_release_zh.md"
    prs_one_liner_by_release_zh = outdir / "prs_one_liner_by_release_zh.md"

    # Index PR rows for fast lookup.
    pr_row_by_id: dict[int, dict] = {int(r["pr"]): r for r in prs_rows}
    pr_to_release: dict[int, str] = {int(r["pr"]): r["release_tag"] for r in prs_rows}

    release_order = [rp["tag"] for rp in release_points]
    prs_in_release: dict[str, list[int]] = defaultdict(list)
    for pr, rel in pr_to_release.items():
        prs_in_release[rel].append(pr)
    for rel in prs_in_release:
        prs_in_release[rel].sort(key=lambda p: (pr_row_by_id[p]["rep_ymd"], pr_row_by_id[p]["rep_order"], p))

    # PR by release summary table.
    with open(prs_by_release_zh, "w", encoding="utf-8") as f:
        f.write("# upstream PR 级别汇总（按 release tag）\n\n")
        f.write(f"- 范围：`{base}..{upstream_ref}`\n")
        f.write(f"- PR 数量（可识别）：{len(prs_rows)}；无 PR 号的 commits：{commits_without_pr}\n\n")

        f.write("| release tag | 日期 | PR 数 | commits | churn(+/-) | Top subsys |\n")
        f.write("|---|---|---:|---:|---:|---|\n")
        for rp in release_points:
            tag = rp["tag"]
            rel_prs = prs_in_release.get(tag, [])
            commits_cnt = int(release_info[tag]["commits"])
            churn = int(release_info[tag]["churn"])
            top_subsys = "-"
            if rel_prs:
                ss = Counter()
                for pr in rel_prs:
                    ss.update(pr_subsys[pr])
                top_subsys = ", ".join([k for k, _ in ss.most_common(4)]) if ss else "-"
            f.write(
                f"| `{tag}` | {rp['date']} | {len(rel_prs)} | {commits_cnt} | {churn} | {top_subsys} |\n"
            )
        f.write("\n")

        f.write("## 每个版本 Top PR（按 churn）\n\n")
        for rp in release_points:
            tag = rp["tag"]
            rel_prs = prs_in_release.get(tag, [])
            if not rel_prs:
                continue
            f.write(f"### {tag}\n\n")
            # Sort PRs by churn desc.
            rel_prs_sorted = sorted(rel_prs, key=lambda p: pr_row_by_id[p]["churn"], reverse=True)
            top_n = 25
            f.write("| PR | 标题 | churn | files | subsys | arch | rep |\n")
            f.write("|---:|---|---:|---:|---|---|---|\n")
            for pr in rel_prs_sorted[:top_n]:
                r = pr_row_by_id[pr]
                arch = r["arch"] or "-"
                rep = r["rep_commit"][:12]
                row_md = (
                    f"| #{pr} | {r['title']} | {r['churn']} | "
                    f"{r['files']} | {r['subsys']} | "
                    f"{arch} | `{rep}` |\n"
                )
                f.write(row_md)
            if len(rel_prs_sorted) > top_n:
                f.write(f"\n- ... +{len(rel_prs_sorted) - top_n} PRs（详见 `prs_detailed_zh.md`）\n\n")
            else:
                f.write("\n")

    # One-liner PR list (grouped by release tag).
    with open(prs_one_liner_by_release_zh, "w", encoding="utf-8") as f:
        f.write("# upstream PR 一句话摘要（按 release tag）\n\n")
        f.write("- 说明：每条为基于 PR 标题的自动中文化归纳，用于快速扫读；精确信息以 PR 链接/代表 commit 为准。\n")
        f.write(f"- 范围：`{base}..{upstream_ref}`\n")
        f.write(f"- PR 数量（可识别）：{len(prs_rows)}；无 PR 号的 commits：{commits_without_pr}\n\n")

        f.write("| release tag | 日期 | PR 数 |\n")
        f.write("|---|---|---:|\n")
        for rp in release_points:
            tag = rp["tag"]
            rel_prs = prs_in_release.get(tag, [])
            f.write(f"| `{tag}` | {rp['date']} | {len(rel_prs)} |\n")
        f.write("\n")

        for rp in release_points:
            tag = rp["tag"]
            rel_prs = prs_in_release.get(tag, [])
            f.write(f"## {tag} ({rp['date']})\n\n")
            if not rel_prs:
                f.write("- （该版本区间无可识别 PR）\n\n")
                continue
            for pr in rel_prs:
                r = pr_row_by_id[pr]
                one = pr_one_liner_zh(r.get("title", ""), r.get("action", ""))
                meta = f"subsys={r.get('subsys') or '-'}; arch={r.get('arch') or '-'}; churn={r.get('churn')}"
                f.write(f"- #{pr} {one}（{meta}；https://github.com/gem5/gem5/pull/{pr}）\n")
            f.write("\n")

    # Detailed PR digest.
    with open(prs_detailed_zh, "w", encoding="utf-8") as f:
        f.write("# upstream PR 逐条摘要（中文结构 + 原始标题）\n\n")
        f.write("- 说明：该文件为“机器生成”的 digest，主要用于快速检索。\n")
        f.write("- PR 的“标题/内容”依赖提交信息；更完整信息需要访问对应 PR 页面。\n\n")
        f.write(f"- 范围：`{base}..{upstream_ref}`\n")
        f.write(f"- PR 数量（可识别）：{len(prs_rows)}；无 PR 号的 commits：{commits_without_pr}\n\n")

        for rp in release_points:
            tag = rp["tag"]
            rel_prs = prs_in_release.get(tag, [])
            if not rel_prs:
                continue
            f.write(f"## {tag} ({rp['date']})\n\n")
            f.write(f"- PR 数：{len(rel_prs)}\n\n")
            for pr in rel_prs:
                r = pr_row_by_id[pr]
                f.write(f"### #{pr} {r['title']}\n\n")
                f.write(f"- 动作（heuristic）: {r['action']}\n")
                f.write(f"- PR 链接: https://github.com/gem5/gem5/pull/{pr}\n")
                f.write(f"- 代表 commit: `{r['rep_commit'][:12]}` ({r['rep_ymd']})\n")
                f.write(
                    "- 变更规模: "
                    f"commits={r['commits']}, files={r['files']}, "
                    f"+{r['insertions']}/-{r['deletions']} "
                    f"(churn={r['churn']})\n"
                )
                f.write(
                    "- 影响范围: "
                    f"topdirs={r['topdirs'] or '-'}; "
                    f"subsys={r['subsys'] or '-'}; "
                    f"arch={r['arch'] or '-'}\n"
                )

                # Top files (by churn).
                fc = pr_file_churn.get(pr, Counter())
                if fc:
                    top_files = fc.most_common(8)
                    f.write("- 主要改动文件（Top 8 by churn）:\n")
                    for path, churn in top_files:
                        f.write(f"  - `{path}` (churn={churn})\n")
                # Commit list (when a PR contains multiple commits).
                commits = pr_to_commits.get(pr, [])
                if len(commits) > 1:
                    f.write(f"- commits 列表（按 topo-order，Top 12）：\n")
                    for c in commits[:12]:
                        f.write(f"  - {commit_ymd.get(c, '')} `{c[:12]}` {commit_title.get(c, '')}\n")
                    if len(commits) > 12:
                        f.write(f"  - ... +{len(commits) - 12} commits\n")
                f.write(f"- 复现: `git show {r['rep_commit']}`\n\n")

    # ==========================
    # Focused subsystem report
    # ==========================

    focus_prefixes: list[tuple[str, str]] = [
        ("src/arch/riscv/", "RISC-V ISA/平台"),
        ("src/cpu/o3/", "O3 CPU"),
        ("src/cpu/pred/", "分支预测/BTB/预测器"),
        ("src/mem/cache/prefetch/", "Prefetcher 相关"),
        ("src/mem/cache/", "Cache 相关（含替换策略等）"),
        ("src/mem/ruby/", "Ruby 内存系统"),
        ("configs/", "配置脚本/示例"),
        ("util/", "工具脚本/资源管理"),
        ("tests/", "测试"),
        (".github/", "CI/工作流"),
    ]

    focus_md = outdir / "focus_subsystems_zh.md"

    with open(focus_md, "w", encoding="utf-8") as f:
        f.write("# 重点目录/子系统总结（相对分叉点）\n\n")
        f.write(f"- 范围：`{base}..{upstream_ref}`\n")
        f.write("- 说明：该报告以“目录前缀”为单位聚合，便于只关注你关心的模块。\n\n")

        for prefix, desc in focus_prefixes:
            # Collect commits/PRs touching this prefix.
            touched_commits: set[str] = set()
            touched_prs: set[int] = set()
            churn_by_pr: Counter = Counter()
            files_counter: Counter = Counter()
            topic_counter: Counter = Counter()

            for r in file_rows:
                if not r["path"].startswith(prefix):
                    continue
                c = r["commit"]
                touched_commits.add(c)
                pr = commit_pr.get(c)
                if pr is not None:
                    touched_prs.add(pr)
                add = r["add"]
                dele = r["del"]
                if add is not None and dele is not None:
                    files_counter[r["path"]] += int(add) + int(dele)
                    if pr is not None:
                        churn_by_pr[pr] += int(add) + int(dele)

            # Topic distribution for this prefix (based on commit subjects).
            if touched_commits:
                sub_df = commits_df[commits_df["commit"].isin(touched_commits)]
                for t, v in sub_df["topic"].value_counts().head(12).items():
                    topic_counter[t] = int(v)

            f.write(f"## `{prefix}` - {desc}\n\n")
            f.write(f"- unique commits: {len(touched_commits)}\n")
            f.write(f"- unique PRs: {len(touched_prs)}\n")
            if topic_counter:
                f.write(f"- Top topics: {', '.join([f'`{k}`({v})' for k, v in topic_counter.most_common(6)])}\n")
            f.write("\n")

            if churn_by_pr:
                f.write("### Top PR（按 churn）\n\n")
                f.write("| PR | 标题 | churn |\n")
                f.write("|---:|---|---:|\n")
                for pr, churn in churn_by_pr.most_common(20):
                    title = pr_row_by_id.get(pr, {}).get("title", "")
                    f.write(f"| #{pr} | {title} | {churn} |\n")
                f.write("\n")

            if files_counter:
                f.write("### Top 文件（按 churn）\n\n")
                f.write("| 文件 | churn |\n")
                f.write("|---|---:|\n")
                for path, churn in files_counter.most_common(20):
                    f.write(f"| `{path}` | {churn} |\n")
                f.write("\n")

    # ==========================
    # Release-tag summary (Chinese)
    # ==========================

    def release_notes_key_for_tag(tag: str) -> Optional[str]:
        """
        Map a git tag (e.g., v25.1.0.0) to a RELEASE-NOTES.md section key (e.g., 25.1).
        """
        if not release_sections:
            return None
        raw = tag.lstrip("v")
        if raw in release_sections:
            return raw
        vt = parse_version_tuple(tag)
        if not vt:
            return None
        # Try major "X.Y"
        if len(vt) >= 2:
            major = f"{vt[0]}.{vt[1]}"
            if major in release_sections:
                return major
        # Try shorter prefixes.
        for n in range(len(vt) - 1, 1, -1):
            cand = ".".join(str(x) for x in vt[:n])
            if cand in release_sections:
                return cand
        return None

    releases_md = outdir / "releases_zh.md"
    with open(releases_md, "w", encoding="utf-8") as f:
        f.write("# upstream release 版本摘要（从分叉点开始）\n\n")
        f.write(f"- 分叉基线：`{base}`（tag: `{base_tag or '-'}`）\n")
        f.write(f"- upstream：`{upstream_ref}`（tag: `{upstream_nearest_tag or '-'}`）\n")
        f.write("- 说明：此文件结合 git tag 时间线与 `RELEASE-NOTES.md` 的内容做中文化梳理。\n\n")

        f.write("## tag 时间线（含别名）\n\n")
        f.write("- 说明：部分 tag 指向同一个 commit（例如 `v25.1` 与 `v25.1.0.0`），属于别名/重复标记。\n\n")
        f.write("| tag | 日期 | commit | subject |\n")
        f.write("|---|---|---|---|\n")
        milestones_sorted = sorted(
            milestones,
            key=lambda m: (m["date"], parse_version_tuple(m["tag"]) or (0,)),
        )
        for m in milestones_sorted:
            f.write(f"| `{m['tag']}` | {m['date']} | `{m['short']}` | {m['subject']} |\n")
        f.write("\n")

        f.write("## 按版本区间统计（去重后）\n\n")
        f.write("| tag | 日期 | commit | 该版本区间 commits | 该版本区间 PR 数 | release-notes key |\n")
        f.write("|---|---|---|---:|---:|---|\n")
        for rp in release_points:
            tag = rp["tag"]
            rn_key = release_notes_key_for_tag(tag) or "-"
            rel_prs = prs_in_release.get(tag, [])
            commits_in_range = int(release_info.get(tag, {}).get("commits", 0))
            row_md = (
                f"| `{tag}` | {rp['date']} | `{rp['short']}` | "
                f"{commits_in_range} | {len(rel_prs)} | "
                f"`{rn_key}` |\n"
            )
            f.write(row_md)
        f.write("\n")

        f.write("## 各版本要点（中文摘要）\n\n")
        for rp in release_points:
            tag = rp["tag"]
            commits_cnt = int(release_info.get(tag, {}).get("commits", 0))
            churn = int(release_info.get(tag, {}).get("churn", 0))
            rel_prs = prs_in_release.get(tag, [])
            rn_key = release_notes_key_for_tag(tag)
            f.write(f"### {tag} ({rp['date']})\n\n")
            f.write(f"- commits: {commits_cnt}\n")
            f.write(f"- PR 数（可识别）：{len(rel_prs)}\n")
            f.write(f"- 变更规模（churn）：{churn}\n")

            # Human-friendly feature bullets (Chinese, best-effort).
            feat_lines: list[str] = []
            if rn_key and rn_key in highlights:
                for h in highlights[rn_key]:
                    zh = _rough_en_to_zh(h).strip()
                    if zh and zh != h:
                        feat_lines.append(f"{zh}（{h}）")
                    else:
                        feat_lines.append(h)
            else:
                # Release-notes based bullets (headings + bullets).
                if rn_key and rn_key in release_sections:
                    feat_lines.extend(summarize_release_section_zh(release_sections[rn_key], max_bullets=6))
                # Add top PRs (by churn) to ensure big features show up even if release-notes
                # are sparse or prose-heavy.
                if rel_prs:
                    top_prs_feat = sorted(rel_prs, key=lambda p: pr_row_by_id[p]["churn"], reverse=True)[:12]
                    for pr in top_prs_feat:
                        r = pr_row_by_id[pr]
                        s = pr_one_liner_zh(r.get("title", ""), r.get("action", ""))
                        if s not in feat_lines:
                            feat_lines.append(s)
                        if len(feat_lines) >= 8:
                            break
            if feat_lines:
                f.write("- 新特性/重要变化（中文归纳，best-effort）：\n")
                for x in feat_lines:
                    f.write(f"  - {x}\n")
            if rn_key and rn_key in highlights:
                f.write("- Major Highlights（摘自 RELEASE-NOTES）：\n")
                for h in highlights[rn_key]:
                    f.write(f"  - {h}\n")
            if rn_key and rn_key in release_sections:
                bullets = summarize_release_section_zh(release_sections[rn_key], max_bullets=10)
                if bullets:
                    f.write("- Release notes 摘要（自动提取）：\n")
                    for b in bullets:
                        f.write(f"  - {b}\n")
            else:
                f.write("- Release notes：未在 `RELEASE-NOTES.md` 中找到对应 section（可能是 tag 别名/合并到主版本说明）。\n")
            if rel_prs:
                top_prs = sorted(rel_prs, key=lambda p: pr_row_by_id[p]["churn"], reverse=True)[:8]
                f.write("- Top PR（按 churn，Top 8）：\n")
                for pr in top_prs:
                    r = pr_row_by_id[pr]
                    f.write(f"  - #{pr} {r['title']} (churn={r['churn']})\n")
            f.write("\n")

    # ==========================
    # Human-friendly entry doc
    # ==========================

    overview_zh = outdir / "overview_zh.md"
    with open(overview_zh, "w", encoding="utf-8") as f:
        f.write("# upstream 变更（人类友好版入口）\n\n")
        f.write("## 你应该先看什么\n\n")
        f.write("- 版本级总结（按 release tag）：`releases_zh.md`\n")
        f.write("- PR 级总结（按 release tag）：`prs_by_release_zh.md`\n")
        f.write("- PR 一句话摘要（按 release tag）：`prs_one_liner_by_release_zh.md`\n")
        f.write("- PR 逐条 digest：`prs_detailed_zh.md`\n")
        f.write("- 重点目录聚合：`focus_subsystems_zh.md`\n")
        f.write("- commit 逐条 digest：`commits_detailed_zh.md`\n\n")

        f.write("## 一句话结论\n\n")
        f.write(
            f"- 从 `{base_tag or 'v?'}` 到 `{upstream_nearest_tag or upstream_ref}`，upstream 主要在 **Arm/RISC-V ISA**、"
            "**标准库/配置脚本**、**Ruby/内存系统**、以及 **CI/工程化** 上持续迭代；"
            f"累计 {total_commits} commits（可识别 PR {len(prs_rows)} 个）。\n\n"
        )

        f.write("## 统计速览\n\n")
        f.write(f"- commits: {total_commits}（merge: {merge_commits}）\n")
        f.write(f"- PR（可识别）: {len(prs_rows)}\n")
        f.write(f"- author 去重: {unique_authors}\n")
        f.write(f"- 与 `{origin_ref}` 对比：origin-only={origin_only}, upstream-only={upstream_only}\n\n")

        f.write("## 可视化\n\n")
        if (fig_dir / "commits_per_month.png").exists():
            f.write("![每月 commits](figures/commits_per_month.png)\n\n")
        if (fig_dir / "prs_per_month.png").exists():
            f.write("![每月 PRs](figures/prs_per_month.png)\n\n")
        if (fig_dir / "top_topics.png").exists():
            f.write("![Top topic 前缀](figures/top_topics.png)\n\n")
        if (fig_dir / "arch_activity.png").exists():
            f.write("![Arch 活跃度](figures/arch_activity.png)\n\n")
        if (fig_dir / "topdirs_activity.png").exists():
            f.write("![Top-level 目录活跃度](figures/topdirs_activity.png)\n\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
