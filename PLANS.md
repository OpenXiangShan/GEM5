# ExecPlan Guide

This document is adapted from [OpenAI's ExecPlan](https://developers.openai.com/cookbook/articles/codex_exec_plans) guidance and tailored to the needs of this repository.

It defines how execution plans (ExecPlans) should be written and maintained in this codebase.

ExecPlans are meant for tasks like these:

- Tasks that cannot realistically be completed in one or two exchanges
- Work that spans multiple steps, files, or experiments
- Investigations that need recorded evidence, decisions, intermediate findings, and current status
- Long efforts where context can easily drift if it is not written down

If the task is just a very small change, a simple bug fix, or a single-file adjustment, a separate ExecPlan is usually unnecessary.

## 1. What an ExecPlan Is

An ExecPlan is not a casual TODO list and not a lightweight checklist.

It is a living execution document that should answer questions like:

- What problem is being solved?
- Why is it worth doing?
- What is already known?
- What exactly happens next?
- How do we know the result is correct?
- What did we learn along the way?
- Why did the plan change midway?

A good ExecPlan should let someone who does not know the previous conversation pick up the task and continue with reasonable confidence.

## 2. When to Use an ExecPlan

Create or update an ExecPlan when one or more of the following is true:

1. The task will likely take significant time or span multiple conversations
2. The work combines at least two of: research, experimentation, debugging, implementation
3. The task touches multiple modules, files, or evidence sources
4. There is meaningful uncertainty and assumptions need to be validated first
5. The task needs a record of why a decision was made, not just what changed
6. The task may be paused and resumed later

Typical examples:

- Aligning gem5 behavior with RTL
- Investigating frontend / BPU / FTQ / flush / redirect behavior
- Analyzing a performance regression
- Landing a larger refactor
- Building a feature that must be implemented in phases
- Prototyping before committing to a final design

## 3. Writing Principles

### 3.0 Language

By default, ExecPlans may be written in Chinese for internal development efficiency.

Use English when the expected audience includes external contributors, or when the plan is intended to be referenced from public-facing documentation, PR discussion, or broader cross-team communication.

### 3.1 Self-Contained

An ExecPlan should be as self-contained as possible.

Do not assume the reader remembers earlier chat history, and do not write things like "same as discussed above".

If background is necessary to move the task forward, write it into the current document.

### 3.2 Outcome-Oriented

Do not stop at "change function X" or "add field Y".

Explain:

- What effect you expect
- What the user or developer should be able to observe afterward
- How that outcome will be validated

### 3.3 Evidence-Oriented

Especially for analysis tasks, do not write guesses as if they were facts.

Clearly separate:

- confirmed facts
- current hypotheses
- open questions
- the evidence supporting a conclusion

### 3.4 Continuously Updated

An ExecPlan is a living document, not a one-time writeup.

As the work progresses, update:

- current status
- new findings
- decision changes
- next actions

### 3.5 Explain Why First

Implementation details can be expanded later, but important decisions must include their rationale.

Someone picking up the task later should be able to understand why this approach was chosen instead of another one.

## 4. Recommended Location

Prefer one ExecPlan file per complex task rather than putting everything into one large document.

By default, ExecPlan instances should be treated as local working documents instead of repository-tracked artifacts. They are useful as intermediate notes during long investigations, but they are often too noisy and task-specific to keep in git by default.

Recommended local directory structure:

```text
docs/
  exec-plans/
    active/
    completed/
    blocked/
```

Meaning:

- `active/`: tasks currently in progress
- `completed/`: finished tasks kept locally for later reference
- `blocked/`: tasks paused pending external conditions

Use concise, descriptive file names, for example:

- `docs/exec-plans/active/gem5-rtl-fetch-align.md`
- `docs/exec-plans/active/bpu-override-investigation.md`
- `docs/exec-plans/active/spec06-regression-debug.md`

Only commit an ExecPlan when there is explicit value in sharing it through the repository, such as cross-person handoff, review, or preserving a decision record that should live with the codebase.

## 5. Recommended Structure

Each ExecPlan should usually contain at least the following sections.

## Title

Use a short sentence that describes the goal.
The title should prefer "action + object" over vague naming.

For example:

- Align gem5 frontend flush behavior with RTL
- Investigate the IPC regression of a SPEC06 benchmark
- Add verifiable observability for BPU override

---

## Background and Goal

Use a few paragraphs to explain:

- what the current problem is
- why it matters
- what result should be achieved
- how that result will be observed

Focus first on the value of the task and the end result, not on implementation details.

---

## Current Known Information

Record the facts, observations, and constraints already confirmed.
This can include:

- relevant modules, files, and paths
- current behavior and how it differs from expectations
- logs, counters, traces, waveforms, or test results already observed
- environment constraints

Do not write guesses as facts in this section.

## Hypotheses and Open Questions

If uncertainty remains, list it explicitly. For example:

- We currently suspect the issue is the timing of override activation
- We are not yet sure whether the second target comes from mainBTB
- We need to verify whether a counter covers the split-request case

The purpose of this section is to keep the analysis from becoming muddled over time.

---

## Planned Steps

List the next steps in order.
Each step should ideally be written as "action + goal + expected output".

For example:

1. Read the frontend redirect path and confirm the actual control flow in gem5
2. Cross-check RTL documentation and implementation, then summarize the flush taxonomy
3. Add the required logs or counters and construct a minimal reproduction
4. Run the chosen workload and verify whether the behavior converges
5. Decide whether to keep the current approach or revise it based on the results

Avoid cryptic shorthand that only the original author can understand.

---

## Validation

Always specify how success will be judged.

Validation may mean:

- tests pass
- logs match expectations
- counters move in the expected direction
- a workload now behaves like RTL
- a performance regression is eliminated
- a scenario is reproducible and then fixed

Even for analysis-only tasks, define what "done" means. For example:

- root cause confirmed
- minimal reproduction identified
- candidate causes ruled out
- a concrete recommendation for the next phase is available

## Progress

Progress must be updated continuously.
Use checkboxes with timestamps.

Example:

- [x] 2026-03-24 10:00 Read the main frontend redirect path and identify the primary entry points
- [x] 2026-03-24 11:20 Cross-check RTL docs and discover that the flush taxonomy differs from the earlier assumption
- [ ] Add counters for the split-request case and verify whether `inflightLoads` fully covers it
- [ ] Construct a minimal workload to validate the second-target selection logic

If a step is only partially complete, say what has been finished and what remains.

---

## Findings and Surprises

Record important new findings that appear during the work.
Especially note things like:

- an earlier understanding was wrong
- docs and code disagree
- a counter definition is unreliable
- a path is more important than expected
- an experiment disproved an earlier hypothesis

This section matters because long tasks often fail when important intermediate learning is not written down.

---

## Decision Log

Whenever an important decision is made or the direction changes, record it.

Recommended format:

- Decision: ...
- Reason: ...
- Date: ...

For example:

- Decision: add observability before changing behavior
- Reason: the root cause is not fully confirmed yet, so changing behavior immediately is too risky
- Date: 2026-03-24
