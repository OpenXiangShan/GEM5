# FDIP Redirect / Partial-State Cleanup Witness (2026-04-22)

## Goal

Provide a narrow Trellis-owned witness that redirect/squash cleanup clears
FDIP partial state from the fetch side instead of relying only on integrated
trace statistics.

## What was added

- Runtime helper:
  - `src/cpu/o3/fdip_cleanup.hh`
- Unit test:
  - `src/cpu/o3/fdip_cleanup.test.cc`
- Build glue:
  - `src/cpu/o3/SConscript`

## Runtime integration

`Fetch::resetFdipPartialState(ThreadID tid)` now uses the shared helper during
the thread-reset / stage-reset / squash path to:

- reset the thread-local FDIP state object
- remove pending FDIP requests for the squashed thread
- decrement outstanding-line accounting for removed in-flight requests
- clear probe hints for that thread

Callsites are in:

- `Fetch::clearStates()`
- `Fetch::resetStage()`
- `Fetch::doSquash()`

## Witness semantics

The unit test proves the cleanup helper does all of the following:

1. Resets the current thread's partial FDIP state.
2. Removes only the current thread's pending FDIP requests.
3. Decrements outstanding-line accounting exactly for requests that were still
   marked outstanding.
4. Clears probe hints associated with the current thread context.
5. Preserves other threads' pending requests.

This is not a full fetch-stage integration harness, but it is a targeted
proof of the cleanup contract used by the real squash path.

## Validation

Completed:

- `scons build/RISCV/cpu/o3/fdip_cleanup.test.opt --unit-test -j8`
- `build/RISCV/cpu/o3/fdip_cleanup.test.opt`

Result:

- 2 / 2 tests passed

## Conclusion

The redirect / partial-state cleanup gap is now covered by a narrow,
maintainable witness:

- old-path partial FDIP state is explicitly cleared on the squash/reset path
- pending in-flight bookkeeping is reclaimed
- probe hints do not survive that cleanup boundary

Remaining related gap:

- there is still no full fetch-stage directed test that drives a real
  cross-line target into mid-flight redirect, but the helper-level cleanup
  contract is now locked down.
