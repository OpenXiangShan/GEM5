# PerfCCT stall-attribution tooling

Tools to analyse and visualise per-instruction pipeline behaviour (stalls,
replays, per-cycle module occupancy) from XS-GEM5's PerfCCT / Arch DB trace.

Two pieces:

- `util/perfcct_report.py` — turns a `lifetime.db` into HTML reports and/or a
  standalone dynamic viewer.
- `util/perfcct_viewer.html` — a **db-agnostic** viewer (SQLite compiled to
  WebAssembly via [sql.js]). Open it once, then load *any* `lifetime.db` from a
  file picker; no regeneration per program.

[sql.js]: https://sql.js.org/

---

## 0. Build gem5 (once, after pulling these changes)

The trace schema was extended (`MemType`, `PAddress`, `ReplayTicks`,
`ExecuteTicks`, and a new `SquashedLifeTimeTrace` table), so the simulator must
be rebuilt before the new columns are written:

```bash
cd GEM5
scons build/RISCV/gem5.opt --gold-linker -j$(nproc)
```

## 1. Produce `lifetime.db`

Run gem5 with the Arch DB enabled. `--enable-arch-db` turns it on and
`--arch-db-file` chooses where the SQLite file is written (instruction
lifetime dumping is on by default):

```bash
export GCBV_REF_SO=`realpath riscv64-nemu-interpreter-*-so`   # difftest ref

mkdir -p m5out/coremark_perfcct
./build/RISCV/gem5.opt configs/example/kmhv3.py \
  --raw-cpt --generic-rv-cpt=./ready-to-run/coremark-2-iteration.bin \
  --enable-arch-db \
  --arch-db-file=m5out/coremark_perfcct/lifetime.db
```

For a different workload just change `--generic-rv-cpt=<bin-or-checkpoint>` and
`--arch-db-file=<path>`.

The db contains three relevant tables:

| table | rows |
|---|---|
| `LifeTimeCommitTrace` | every committed instruction, all stage ticks + stall attribution |
| `LoadLifeTimeCommitTrace` | loads only: vaddr/paddr, replay type string, replay/execute ticks |
| `SquashedLifeTimeTrace` | wrong-path (squashed) instructions' partial lifecycle |

## 2. Generate the viewer (and optional static reports)

```bash
python3 util/perfcct_report.py m5out/coremark_perfcct/lifetime.db \
  -o m5out/coremark_perfcct/coremark \
  --viewer --part detail
```

Outputs (prefixed by `-o`):

- `coremark_viewer.html` — **main tool**, dynamic, loads the whole db on demand.
- `coremark_detail.html` — static per-cycle + per-instruction report.
- with `--part both`: also `coremark_overview.html` — program-wide charts.

Useful options (baked into the viewer as defaults; still editable in its
header at runtime):

| option | meaning | default |
|---|---|---|
| `-p, --period` | ticks per cycle (3 GHz = 333, 2 GHz = 500) | 333 |
| `--io-method` | device/MMIO detection: `addr` (paddr < dram-base) or `lat` (latency) | `addr` |
| `--dram-base` | physical DRAM base; loads below it are device/MMIO | `0x80000000` |
| `--part` | `overview` / `detail` / `both` | `both` |
| `--viewer` | also emit the standalone dynamic viewer | off |

## 3. Open the viewer and load a db

1. Open `coremark_viewer.html` in a browser.
2. Use the file picker to select a `lifetime.db`.
3. Explore: two per-cycle module-occupancy panels (incl. squashed insts), a
   cycle slider, and a paginated per-instruction table with filter / sort /
   expand.

> The viewer pulls sql.js from a CDN (`cdnjs.cloudflare.com`), so the **first**
> open needs internet access.

### Switching workloads (important)

The viewer is **not** tied to a program. To analyse another test you only need
a new `lifetime.db` (rerun step 1); reuse the **same** `*_viewer.html` and pick
the new db. Re-run step 2 only when you want to change the baked-in defaults or
you edited `util/perfcct_viewer.html` itself.

---

## Per-instruction table: column legend

All timing columns except `absF` are **relative to that instruction's own
fetch** (value = the stage's absolute cycle − fetch cycle). Only `absF` is an
absolute cycle. Blue cells jump the cycle panels to the corresponding absolute
cycle.

| column | meaning |
|---|---|
| `ID` | commit-order sequence number. Row prefix ▶/▼ = collapsed/expanded per-cycle stall timeline (click the row); ⟳ = replayed load |
| `absF` | absolute cycle the inst was fetched (AtFetch) |
| `PC` | program counter |
| `dec` | cycles from fetch to Decode |
| `ren` | cycles from fetch to Rename |
| `isq` | cycles from fetch to entering the Issue Queue |
| `fu` | cycles from fetch to start of execution (AtFU); last pass for replayed loads |
| `wb` | cycles from fetch to write-back (AtWriteVal) |
| `cmt` | cycles from fetch to commit (AtCommit) |
| `tot` | total lifetime = AtCommit − AtFetch |
| `Reason(real)` | real stall reason; if raw reason is `HoLBlocked` it is resolved to the `SecondaryReason` (what the ROB head was actually stuck on) |
| `sCyc` | `StallCycles`: cycles charged to this instruction as a stall |
| `rawDominant` | raw dominant `StallReason` (may still be `HoLBlocked`) |
| `disasm` | disassembled instruction |

## Device / MMIO I/O handling

UART / device polling shows up as loads with huge latency and would otherwise
dominate the stats. Detection defaults to the **address method**: a load whose
physical address is below `--dram-base` (default `0x80000000`) is device/MMIO.
These loads (and the cycles they occupy) are excluded from counts and from the
cycle slider, so the numbers reflect real CPU work. A latency-based fallback is
available (`--io-method lat`).

## Notes on the simulator-side changes

- Exact load replay type is recorded per pass (TLBMiss / CacheMiss / Reschedule
  / STLF / Nuke / BankConflict / RAR / RAW / ...), instead of lumping most into
  "Other".
- `HoLBlocked` is only charged once a load is *genuinely* done (not
  `needReplay()` and not `inPipe()`), so a speculatively-CanCommit load that is
  still replaying is not mislabelled as "ready & waiting".
- Squashed instructions' partial lifecycles are dumped so the per-cycle module
  view can show wrong-path activity.
