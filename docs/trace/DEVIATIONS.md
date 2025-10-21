Trace Mode Deviations (for rollback)

Scope: Restore O3 pipeline to normal semantics; keep only frontend trace reader and noSquashFromTC behavior per approval.

- commit.cc
  - Keep noSquashFromTC-related changes (approved to retain for now).
  - Revert other trace-only relaxations if any arise later.

- rob.cc
  - Revert relaxed isInROB() assertion (trace-mode exception).

- mem_dep_unit.cc
  - Revert squash-time memDepHash relaxed assertion (trace-mode exception).

- issue_queue.cc / issue_queue.hh
  - Remove replay fast-path to execute queue (notifyReplayIssued/iqOwner back-ref).
  - Remove trace-only double-issue skip and bypass relaxations where present.

- inst_queue.cc / inst_queue.hh
  - Remove enqueueToExecute() fast-path and related scheduler hooks.
  - Remove trace-only guards that skip cache-miss retry for already executed loads.

- iew.cc
  - Restore scheduleReadyInsts() placement under exeStatus!=Squashing.
  - Restore LSQ executePipeSx() to run after executing issued instructions, not before.
  - Remove trace-only observe/skip paths in load execution.

- lsq.cc
  - Remove TimingResp direct-complete path (scheduleWriteback on resp) in trace mode.

- lsq_unit.cc
  - Remove direct-complete/cancel in-pipe path in WritebackRegEvent.
  - Restore last-stage readyToFinish behavior to normal semantics.

Notes
- Frontend trace reader (fetch.*), metadata map, and CPU::isTraceMode remain.
- noSquashFromTC behavior is retained as requested.

