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


def extract_pr_number(subject: str) -> Optional[int]:
    m = re.search(r"\(#(\d+)\)\s*$", subject)
    if m:
        try:
            return int(m.group(1))
        except ValueError:
            return None
    return None


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

    # Release tag milestones between base tag and upstream nearest tag (if available).
    def parse_ver(tag: str) -> Optional[tuple[int, ...]]:
        if not tag.startswith("v"):
            return None
        parts = tag[1:].split(".")
        if not parts or not all(p.isdigit() for p in parts):
            return None
        return tuple(int(p) for p in parts)

    base_ver = parse_ver(base_tag) if base_tag else None
    upstream_ver = parse_ver(upstream_nearest_tag) if upstream_nearest_tag else None
    milestones: list[dict] = []
    try:
        merged_tags = run_git(repo, ["tag", "--merged", upstream_ref]).splitlines()
    except subprocess.CalledProcessError:
        merged_tags = []
    cand_tags: list[str] = []
    for t in merged_tags:
        v = parse_ver(t.strip())
        if not v:
            continue
        if base_ver and v < base_ver:
            continue
        if upstream_ver and v > upstream_ver:
            continue
        cand_tags.append(t.strip())
    cand_tags = sorted(set(cand_tags), key=lambda t: parse_ver(t) or (0,))
    # Keep it readable: only versioned tags in this range are already small (< ~30).
    for t in cand_tags:
        try:
            # Peel annotated tags to the commit they reference.
            commit = run_git(repo, ["rev-parse", f"{t}^{{}}"]).strip()
            info = run_git(repo, ["show", "-s", "--date=short", "--format=%ad%x1f%s", commit]).strip()
            d, s = info.split(FIELD_SEP)
            milestones.append({"tag": t, "date": d, "commit": commit[:12], "subject": s})
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
                f.write(f"| `{m['tag']}` | {m['date']} | `{m['commit']}` | {m['subject']} |\n")
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
        f.write("- `data/commits.csv`: commit metadata + aggregated numstat\n")
        f.write("- `data/files.csv`: per-file numstat (may be large)\n")
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
                f.write(f"| `{m['tag']}` | {m['date']} | `{m['commit']}` | {m['subject']} |\n")
            f.write("\n")

        f.write("## 可视化\n\n")
        if (fig_dir / "commits_per_month.png").exists():
            f.write("![每月 commit 数](figures/commits_per_month.png)\n\n")
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
        f.write("- 原始数据：`data/commits.csv`（commit 元信息 + 聚合 numstat）\n")
        f.write("- 原始数据：`data/files.csv`（逐文件 numstat，体积较大）\n")

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
                f"- {row.ymd} `{row.short}`{merge_mark} {row.subject} "
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
            pr = extract_pr_number(row.subject)
            action = classify_action(row.subject)
            archs = format_set(commit_to_arch.get(row.commit, set()))
            subsys = format_set(commit_to_subsys.get(row.commit, set()))
            topdirs = format_set(commit_to_topdir.get(row.commit, set()))

            f.write(f"### {row.ymd} `{row.short}` {merge_mark} {row.subject}\n\n")
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

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
