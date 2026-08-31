---
name: Performance Anomaly Investigator (DeepSeek)

on:
  workflow_dispatch:
    inputs:
      monitor_run_id:
        description: Performance CI Monitor run containing perf-monitor-analysis
        required: true
        type: string
      source_run_id:
        description: Original GEM5 performance workflow run
        required: true
        type: string

permissions:
  actions: read
  contents: read

runs-on: [self-hosted, node]
timeout-minutes: 30
max-ai-credits: 2000

engine:
  id: codex
  env:
    OPENAI_BASE_URL: https://api.deepseek.com
    OPENAI_API_KEY: ${{ secrets.DEEPSEEK_API_KEY }}
model: deepseek-v4-flash

network:
  allowed:
    - defaults
    - api.deepseek.com

sandbox:
  agent:
    model-fallback: false
    token-steering: false

safe-outputs:
  staged: true
  report-failure-as-issue: false
  create-issue:
    title-prefix: "[perf-monitor preview] "
    max: 1

steps:
  - name: Download deterministic performance analysis
    env:
      GH_TOKEN: ${{ github.token }}
      MONITOR_RUN_ID: ${{ inputs.monitor_run_id }}
    run: |
      mkdir -p "$GITHUB_WORKSPACE/perf-analysis"
      gh run download "$MONITOR_RUN_ID" \
        --repo "$GITHUB_REPOSITORY" \
        --name perf-monitor-analysis \
        --dir "$GITHUB_WORKSPACE/perf-analysis"
      test -s "$GITHUB_WORKSPACE/perf-analysis/analysis.json"
---

You are investigating a completed GEM5 performance CI anomaly. This is an
analysis-only shadow-mode workflow.

Start with `perf-analysis/analysis.json` and `perf-analysis/summary.md`. Treat
the deterministic completeness checks, score deltas, and weighted counter
deltas as the primary evidence. The original source run is
`${{ inputs.source_run_id }}`.

If more evidence is needed:

1. Read the candidate and baseline NFS archive paths from `analysis.json`.
2. Inspect at most the three most anomalous workloads first.
3. Prefer normalized rates, MPKI, weighted counters, and Topdown fractions over
   raw counts. Check committed instructions and coverage before interpreting a
   raw counter delta.
4. Inspect the git diff between the baseline and candidate commits and connect
   changed modules to the observed counters. Fetch the exact SHAs recorded in
   `analysis.json` if the shallow checkout does not contain them. Clearly
   distinguish confirmed evidence, plausible hypotheses, and missing data.
5. For a failed CI run, identify whether the failure is build, infrastructure,
   timeout, abort/difftest, incomplete archive, or score-processing related.

Do not modify the checkout or NFS data. Do not create commits, branches, pull
requests, comments, or issues. Do not attempt to repair the code. Never print
credentials or environment secrets.

Produce a concise Chinese report containing:

- candidate/baseline identity and comparison validity;
- CI health and completeness;
- score and workload anomalies;
- the most relevant counter evidence;
- likely source-level causes and the smallest follow-up checks.

Finish by calling the `noop` safe-output tool with a short completion message.
All configured write outputs are staged previews and must not be published.
