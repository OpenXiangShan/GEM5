# Architecture

This document is a high-level map of XS-GEM5 as it exists in this repository today.
It is intentionally biased toward the parts we actively evolve: the XiangShan-aligned O3 CPU, the decoupled frontend, and the configuration surface that shapes timing behavior.

This is not an exhaustive design spec and it is not a line-by-line code tour.
The source code remains the ground truth.
Use this document to answer three questions quickly:

1. Where does a behavior live?
2. What are the important boundaries and invariants?
3. Which files should I read before changing something?

This document is inspired by the style of architecture notes that act as repository maps rather than encyclopedias:

- https://matklad.github.io/2021/02/06/ARCHITECTURE.md.html
- https://raw.githubusercontent.com/rust-lang/rust-analyzer/d7c99931d05e3723d878bea5dc26766791fa4e69/docs/dev/architecture.md
- https://openai.com/index/harness-engineering/

## Scope

XS-GEM5 is a gem5-based RISC-V simulator calibrated against XiangShan RTL.
In this fork, accuracy for XiangShan-style full-system and checkpoint-driven studies matters more than preserving the most generic upstream gem5 structure.

If you are new to the repository, keep these points in mind first:

- This repository is primarily a full-system simulator, not a general-purpose SE-first environment.
- The most important development area is the O3 CPU and its surrounding frontend/backend timing model.
- `configs/example/kmhv3.py` is not "just a demo config"; it is one of the main architectural entry points because it selects the RTL-aligned microarchitectural shape.
- Some upstream abstractions still exist, but the active XiangShan path is narrower than upstream gem5's full design space.

## Bird's-Eye View

At a high level, the system looks like this:

```text
configs/example/kmhv3.py
        |
        v
src/cpu/o3/BaseO3CPU.py  +  src/cpu/pred/BranchPredictor.py
        |
        v
    gem5::o3::CPU
        |
        +--> Fetch  --> Decode --> Rename --> IEW --> Commit
        |       ^                                  |
        |       |                                  |
        |       +------ backward control ----------+
        |
        +--> Branch predictor, decoder, ROB, IQ, LSQ, regfile, scoreboard
        |
        +--> Caches / memory hierarchy / difftest / stats / tracing
```

The important practical point is that "architecture" in this repository is split across three layers:

- Python configuration defines the chosen machine shape.
- C++ pipeline stages implement the timing and state transitions.
- Documentation and debug tooling explain how to observe and validate the model.

If you only read one layer, you will usually misunderstand the behavior.

## Code Map

The following directories matter most for current work:

- `src/cpu/o3/`
  - The main O3 core model.
  - Start here for pipeline timing, recovery, register renaming, ROB, IQ, LSQ, and stage orchestration.
- `src/cpu/pred/`
  - Branch prediction subsystem.
  - In the XiangShan-aligned path, the decoupled BTB-based frontend is the most relevant implementation.
- `src/arch/riscv/`
  - ISA-specific decode, PC state, faults, and RISC-V architectural behavior.
- `configs/example/kmhv3.py`
  - RTL-aligned Kunminghu V3 configuration.
  - This file selects widths, queue sizes, predictor options, cache details, and many alignment switches.
- `configs/example/idealkmhv3.py`
  - Ideal/performance-tuned Kunminghu V3 configuration, for performance tuning.
- `src/cpu/o3/BaseO3CPU.py`
  - The parameter surface of the O3 core.
  - If a behavior looks "hard-coded", verify whether it is actually parameterized here.
- `src/cpu/pred/BranchPredictor.py`
  - Predictor object graph and parameter definitions.
- `README.md`
  - Project positioning, supported workflows, and what this fork is trying to optimize for.

Useful supporting documents already inside the repository:

- `src/cpu/pred/README.md`
- `src/cpu/o3/fetch.md`
- `docs/design-docs/frontend/README.md`
  - Design-oriented Kunminghu v3 frontend/BPU notes.
  - Prefer this directory when you need "why / constraints / tradeoffs".
- `docs/design-docs/smt/README.md`
  - Design-oriented SMT notes covering thread isolation, shared resources,
    recovery, memory visibility, and full-system validation.
- `docs/Gem5_Docs/`
  - More detailed implementation notes and local deep dives.

## The O3 Mental Model

The O3 CPU is not a monolith.
`gem5::o3::CPU` composes a set of pipeline stages plus the shared structures they communicate through.

The most important files are:

- `src/cpu/o3/cpu.hh`
- `src/cpu/o3/cpu.cc`
- `src/cpu/o3/comm.hh`
- `src/cpu/o3/dyn_inst.hh`

The constructor in `src/cpu/o3/cpu.cc` is especially important because it wires:

- stage ownership
- shared structures such as the ROB, scoreboard, and register file
- forward queues between stages
- backward `TimeBuffer` links
- active-thread lists and stage cross-references

If you are unsure how two stages are connected, start in the CPU constructor before diving into stage-local code.

### Stage orchestration

`CPU::tick()` drives the pipeline in reverse stage order:

```text
Commit -> IEW -> Rename -> Decode -> Fetch
```

Only after all stage `tick()` calls complete do the time buffers advance.

**Architecture invariant:** stage order inside a cycle matters.
Do not reason about the pipeline as if all stages update simultaneously.
If you change a signal path, always ask:

- Which stage writes it?
- Which delayed wire reads it?
- In which later cycle does it become visible?

### Communication model

The O3 core uses two kinds of communication objects:

- Forward queues such as `FetchStruct`, `DecodeStruct`, `RenameStruct`, and `IEWStruct`
- Backward communication via `TimeBuffer<TimeStruct>`

`src/cpu/o3/comm.hh` is therefore a key architectural file, not just a collection of structs.

**Architecture invariant:** if a change affects pipeline control, it is usually a `comm.hh` problem before it is a stage-local problem.

### The unit of execution: `DynInst`

`DynInst` is the dynamic instruction object carried through the pipeline.
It owns the per-instance state that the stages mutate:

- sequence number
- decoded/static instruction
- renamed registers
- ROB / LSQ / scheduler status
- squash / execute / commit state
- XiangShan-specific metadata (`xsMeta`)
- pipeline timestamps used for tracing and analysis

**Architecture invariant:** most interesting O3 behavior is expressed as state transitions on `DynInst`, not as isolated logic inside one stage.

If a bug spans multiple stages, inspect the instruction state first.

## Frontend Architecture

The frontend is where this repository diverges most strongly from a "textbook upstream gem5 O3" mental model.

### Fetch is built around a decoupled predictor

`src/cpu/o3/fetch.cc` currently assumes a decoupled BTB-based predictor path.
The constructor explicitly asserts that the predictor exists, is decoupled, and is BTB-based.

**Architecture invariant:** for the active XiangShan path, fetch should be understood as consuming a decoupled frontend supply, not as running a generic legacy predictor interface.

In practice, the flow is:

```text
Decoupled predictor stages
    -> FSQ (same as fetch target queue/FTQ)
    -> I-cache access
    -> instruction bytes
    -> DynInst creation
    -> FetchStruct to Decode
```

This means frontend changes often span both:

- `src/cpu/pred/btb/`
- `src/cpu/o3/fetch.*`

Changing only one side is often incomplete.

### Branch prediction is a composed subsystem

The active BTB frontend is not a single predictor.
It is a composition of predictors defined through `BranchPredictor.py` and implemented in `src/cpu/pred/btb/`.
For Kunminghu V3, the important components include:

- UBTB
- ABTB / MBTB
- MicroTAGE / TAGE
- ITTAGE
- MGSC
- RAS

The predictor produces higher-level fetch supply structures such as fetch streams and fetch targets.
For orientation, `src/cpu/pred/README.md` is a useful companion.
For design rationale of the active Kunminghu v3 BTB path, read `docs/design-docs/frontend/README.md` before diving into individual predictor files.

**Architecture invariant:** predictor state, fetch supply state, and commit-time training state are separate concepts.
Do not collapse them mentally into "the BPU".

### Decode does less than the name suggests

In this O3 design, many instructions are effectively decoded when `StaticInst` objects are created in fetch.
`Decode` still matters, but more for:

- validating fetched instructions
- handling PC-relative correctness
- fusion logic
- forwarding instructions to rename
- propagating stalls and self-squash conditions

If you come from RTL, do not assume decode here corresponds one-to-one with a hardware decode stage.
It is a simulator pipeline stage with slightly different responsibilities.

## Backend Architecture

The backend is split across `Rename`, `IEW`, and `Commit`, plus shared structures like the register file, free list, ROB, IQ, LSQ, and scoreboard.

### Rename

Relevant files:

- `src/cpu/o3/rename.hh`
- `src/cpu/o3/rename.cc`
- `src/cpu/o3/rename_map.hh`
- `src/cpu/o3/free_list.hh`

Rename is responsible for:

- mapping architectural registers to physical registers
- recording rename history for squash recovery
- checking resource pressure before instructions advance
- coordinating with ROB / IQ / LSQ capacity

**Architecture invariant:** rename is the place where speculative architectural intent becomes backend resource ownership.

### IEW

Relevant files:

- `src/cpu/o3/iew.hh`
- `src/cpu/o3/iew.cc`
- `src/cpu/o3/inst_queue.hh`
- `src/cpu/o3/lsq.hh`
- `src/cpu/o3/lsq_unit.hh`

IEW combines several concerns:

- dispatch into backend queues
- issue scheduling
- execution and FU timing
- memory execution through the LSQ
- writeback and wakeup

This is one of the densest parts of the model because scheduler behavior, memory replay behavior, and completion timing meet here.

**Architecture invariant:** if performance moves unexpectedly, IEW is often where the hidden coupling lives.
Queue pressure, replay behavior, wakeup timing, and scoreboard readiness all interact here.

### Commit

Relevant files:

- `src/cpu/o3/commit.hh`
- `src/cpu/o3/commit.cc`
- `src/cpu/o3/rob.hh`
- `src/cpu/o3/rob.cc`

Commit is not just retirement.
It is also the global authority for precise recovery.
It owns:

- in-order architectural retirement
- freeing speculative resources
- committing predictor state
- handling backend-initiated redirects
- driving squash and recovery through the rest of the pipeline

**Architecture invariant:** commit is the recovery authority.
When control flow, precise exceptions, or architectural state disagree, commit is the first place to verify the intended truth.

## Memory-Side Timing

For XiangShan-aligned work, the memory side is part of the CPU architecture, not a detached subsystem.

The most relevant pieces are:

- LSQ and LSQUnit in `src/cpu/o3/`
- load/store replay paths
- store buffer behavior
- bank-conflict and dependency checks
- cache and bus parameters selected by the configuration script

This is especially important because many user-visible performance deltas that look like "frontend issues" are actually replay, conflict, or completion-width effects in the backend memory path.

## Configuration Is Part of the Architecture

In this repository, Python configuration is not just wiring.
It is where the chosen machine instance becomes concrete.

Two files matter especially:

- `src/cpu/o3/BaseO3CPU.py`
- `configs/example/kmhv3.py`

`BaseO3CPU.py` defines the parameter surface:

- widths
- delays
- queue sizes
- register counts
- ROB and squash policy
- LSQ and replay knobs
- predictor choice

`kmhv3.py` then selects the XiangShan-aligned point in that design space.

**Architecture invariant:** before changing C++, check whether the current behavior is actually selected by configuration.

Many "bugs" are really mismatches between:

- the chosen config
- the expected RTL point
- the developer's assumed default behavior

## RISC-V Boundary

The O3 CPU is generic in shape, but the active product is RISC-V and XiangShan-oriented.
That means `src/arch/riscv/` matters whenever behavior depends on:

- decode and instruction semantics
- PC state transitions
- traps and faults
- CSR or privilege interactions
- ISA-specific execution details

When frontend or commit behavior looks wrong, do not stop at `src/cpu/o3/`.
The bug may live in ISA semantics rather than pipeline plumbing.

## What We Optimize For

This repository does not optimize for the same thing everywhere.
The current priorities are roughly:

1. XiangShan alignment
2. Timing-model usefulness for performance work
3. Debuggability and traceability
4. Upstream elegance only where it does not fight the above goals

This has a practical implication:

**Architecture invariant:** do not "simplify" code by removing alignment-driven structure unless you also verify that the resulting model still matches the intended XiangShan behavior.

## How To Navigate a Change

If you are about to modify something, start from the user-visible symptom and walk inward:

### If the issue is in frontend behavior

Read in this order:

1. `configs/example/kmhv3.py`
2. `src/cpu/pred/README.md`
3. `src/cpu/pred/BranchPredictor.py`
4. `src/cpu/pred/btb/*`
5. `src/cpu/o3/fetch.hh` and `src/cpu/o3/fetch.cc`
6. `src/arch/riscv/decoder.cc` if decode semantics are involved

### If the issue is in backend performance or correctness

Read in this order:

1. `src/cpu/o3/comm.hh`
2. `src/cpu/o3/dyn_inst.hh`
3. `src/cpu/o3/rename.*`
4. `src/cpu/o3/iew.*`
5. `src/cpu/o3/inst_queue.*`
6. `src/cpu/o3/lsq*`
7. `src/cpu/o3/commit.*`
8. `src/cpu/o3/rob.*`

### If the issue is "the model shape is wrong"

Check configuration before implementation:

1. `src/cpu/o3/BaseO3CPU.py`
2. `configs/common/xiangshan.py`
3. `configs/example/kmhv3.py`
4. `src/cpu/pred/BranchPredictor.py`

## Debugging and Validation

A few habits work well in this repository:

- Follow the pipeline through `comm.hh` and `DynInst` state instead of reading stage files in isolation.
- Treat DPRINTF traces as architectural observability, especially for fetch, decoupled BPU, IEW, LSQ, and commit.
- Validate both correctness and timing shape; a change that "works" may still be wrong for RTL alignment.
- For predictor work, use the existing BTB unit tests when possible, especially under `src/cpu/pred/btb/test/`.
- For larger behavior changes, validate with the XiangShan-style checkpoint workflow rather than a minimal synthetic path only.

## In One Sentence

XS-GEM5 should be read as a configured, XiangShan-oriented machine model where Python configuration, decoupled frontend state, O3 pipeline timing, and commit-driven recovery together define the architecture.
