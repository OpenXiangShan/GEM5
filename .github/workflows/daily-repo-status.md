---
description: |
  This workflow creates concise daily repository status reports in Chinese.
  It summarizes the last 24 hours of repository activity and highlights
  blockers and actionable next steps for maintainers.

on:
  schedule:
    # Every day at 09:00 Asia/Shanghai (01:00 UTC).
    - cron: "0 1 * * *"
  workflow_dispatch:

permissions:
  contents: read
  issues: read
  pull-requests: read

network:
  allowed:
    - defaults
    - api.deepseek.com

tools:
  bash: ["cat", "ls", "find", "grep", "head", "tail", "wc", "jq", "mkdir"]
  github:
    # If in a public repo, setting `lockdown: false` allows
    # reading issues, pull requests and comments from 3rd-parties
    # If in a private repo this has no particular effect.
    lockdown: false
    min-integrity: none # This workflow is allowed to examine and comment on any issues

safe-outputs:
  report-failure-as-issue: false
  mentions: false
  allowed-github-references: []
  create-issue:
    title-prefix: "[repo-status] "
    labels: [report]
    close-older-issues: true
engine:
  id: copilot
  env:
    COPILOT_PROVIDER_BASE_URL: https://api.deepseek.com
    COPILOT_PROVIDER_API_KEY: ${{ secrets.DEEPSEEK_API_KEY }}
    COPILOT_PROVIDER_TYPE: openai
    COPILOT_PROVIDER_WIRE_API: completions
    COPILOT_MODEL: deepseek-chat
model: deepseek-chat

source: githubnext/agentics/workflows/repo-status.md@578e0e0ea6291fed42a36d3fd46cec6a0e86afd8
---

# Repo Status

Create a concise daily status report for the repo as a GitHub issue.
Write the title and body in Simplified Chinese. Preserve technical identifiers,
PR titles, and links when useful. Use a title such as "仓库日报：YYYY-MM-DD".

## What to include

- Changes from the 24 hours preceding this run: merges, new or materially
  updated PRs/issues, releases, and relevant CI results. State the exact time
  window in Asia/Shanghai and activity counts before selecting highlights.
- Up to eight important changes, grouped by subsystem where useful, with links
  and a brief explanation of impact. Prioritize merged changes, substantive
  code/review updates, and newly resolved or introduced blockers.
- Include a compact linked list of other materially updated PRs/issues that
  did not fit the highlights. Do not present selected highlights as all activity.
- Current blockers and at most three concrete next steps for maintainers.
- Distinguish author-reported validation from independently verified CI results.
  Match CI evidence to the PR head SHA or explicitly identify an older commit;
  skipped jobs do not establish validation. Do not infer blockers from a missing
  test alone or carry forward stale blockers without checking recent updates.
- Do not repeat the entire open PR backlog or old merges. Exclude this workflow's
  own status reports, failure issues, and automation-only PR changes from the
  activity summary. Label-only changes and routine bot chatter are not substantive.

## Style

- Keep the report factual and easy to scan; aim for 600-1200 Chinese characters,
  allowing a compact additional activity list when needed for coverage.
- Avoid generic encouragement, repeated goal reminders, and decorative emojis.
- If there are no meaningful changes, emit `noop` with a brief reason.

## Process

1. Establish the rolling 24-hour cutoff. Use separate bounded GitHub MCP queries
   for recently merged PRs, newly created PRs/issues, and updated PRs/issues across
   all states. Paginate each result to completion, deduplicate by number, and read
   relevant bodies, recent commits, reviews/comments, and CI evidence before
   summarizing. Do not use a capped list of open PRs as the activity source.
   Select only needed fields; avoid full repository scans and unauthenticated
   shell GitHub commands. If retrieval is incomplete, state the coverage gap.
2. Compose the report and call the `create_issue` safe-output MCP tool directly
   with the final title and body. Prefer this over constructing a shell payload.
3. If the CLI transport is necessary, use a temporary Markdown file and the
   authorized `jq -Rs` command to construct valid JSON for `safeoutputs`.
4. Never retry the same permission-denied operation more than twice. Switch to
   the direct safe-output MCP tool, or report `missing_tool` and stop if blocked.
5. Trust the successful safe-output response; do not poll for the issue before
   the downstream publishing step has run.
