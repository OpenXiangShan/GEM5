# CCHI (CHIron Cohestra) Integration — Build & Run Guide

This directory integrates CHIron's Cohestra CCHI fabric into XS-GEM5: each
core's classic L1 caches (untouched, with their prefetchers and the L1
performance model intact) attach to a `CCHIL1Agent` (a CHIron Taurus upstream
node), and all agents share one `CCHIFabric` with a pluggable downstream home:

- **Earth** — vendored C++ behavioral home (`src/mem/cchi/earth/`), no RTL
  toolchain needed.
- **RTL** — any Verilator-model home plugged in through CHIron's V3 pin
  bindings. Validated with **Venus** (`venus_v3_top_extmem`) from the CHIron
  checkout; user-specified RTL tops are supported via `CCHI_RTL_TOP` /
  `CCHI_RTL_SRCS`.

Everything is compile-time gated (`WITH_CCHI`, `WITH_CCHI_RTL`) and runtime
gated (`--cchi`): the stock `build/RISCV` binary and the default kmhv3
configuration are bit-identical with or without this code present.

## Prerequisites

- **CHIron checkout** (read-only; never modified by the build). Located by,
  in priority order: the `CHIRON_DIR` scons variable or the `$CHIRON_DIR`
  environment variable. There is no default location: an unset `CHIRON_DIR`
  fails the build with an explicit error.
- **Verilator** on `PATH` (RTL builds only).
- **scons** via the repo venv: `.venv/bin/scons`.
- **Workloads**: binaries from the OpenXiangShan `ready-to-run` repo (not
  part of this repository; supply its location per run via `READY_TO_RUN`,
  `--generic-rv-cpt` and `GCBV_REF_SO`):
  - `coremark-2-iteration.bin`, `linux.bin`, `microbench.bin` — raw binaries
  - `riscv64-nemu-interpreter-so` — difftest reference (`GCBV_REF_SO`)

## Build

All CCHI options are *sticky* per build directory: pass them once on the
first configure; later rebuilds of the same variant need no flags.

```bash
# Baseline (no CCHI code at all)
.venv/bin/scons build/RISCV/gem5.opt --gold-linker -j$(nproc)

# Earth (C++ home): WITH_CCHI=True
.venv/bin/scons build/RISCV_CCHI/gem5.opt --gold-linker -j$(nproc) \
    WITH_CCHI=True CHIRON_DIR=/path/to/CHIron

# RTL home (Verilator): WITH_CCHI_RTL=True + a top
.venv/bin/scons build/RISCV_CCHI_RTL/gem5.opt --gold-linker -j$(nproc) \
    WITH_CCHI=True WITH_CCHI_RTL=True CCHI_RTL_TOP=venus_v3_top_extmem

# XSCache home (Oceanus L2 + OpenLLC + OpenNCB): dedicated variant;
# the XSCache checkout path is a per-build knob, never a repo default
.venv/bin/scons build/RISCV_CCHI_XSCACHE/gem5.opt --gold-linker -j$(nproc) \
    CCHI_XSCACHE_DIR=/path/to/XSCache    # or: export XSCACHE_DIR=...
```

Notes:

- Known-top shortcuts: `top_dummy` (loopback smoke top) and `venus_v3_top` /
  `venus_v3_top_extmem` (sources auto-resolved from the CHIron checkout),
  and `TestTop_L2OpenLLC` (sources globbed from the XSCache checkout given
  by `CCHI_XSCACHE_DIR`/`$XSCACHE_DIR` — see "XSCache downstream" below).
  Anything else needs an explicit `CCHI_RTL_SRCS` (see "User-specified
  RTL" below).
- Verilation runs at **configure time** into the machine-local temp dir
  `/tmp/cchi_rtl_<top>_<hash>/obj_dir` — deliberately *not* the build tree
  (some network filesystems drop freshly written build-tree files). A stamp
  of the inputs (top, Verilator version, source paths + mtimes/sizes) skips
  re-verilation on plain rebuilds; regenerating a developing RTL checkout
  (e.g. XSCache's `make test-top-l2openllc`) re-verilates automatically.
  Changing `CCHI_RTL_TOP` on an existing variant wipes that variant's
  objects and rebuilds.
- If a build appears to succeed but the binary is stale, re-run with the
  exit code visible: `... ; echo "SCONS_EXIT=${PIPESTATUS[0]}"` and grep the
  log — piping scons through `tail` can swallow compiler errors.

## Run

### Functional memtest (no RISC-V binary needed)

`configs/example/cchi_memtest.py`: MemTest traffic generator -> classic L1D
-> `CCHIL1Agent` -> `CCHIFabric` (with CHIron's `CacheLineDataMonitor`
data-integrity scoreboard) -> membus -> DRAM.

```bash
# Earth, 1 core
./build/RISCV_CCHI/gem5.opt configs/example/cchi_memtest.py \
    --max-loads 200000

# Earth, 4 cores (exercises home snoop paths)
./build/RISCV_CCHI/gem5.opt configs/example/cchi_memtest.py \
    --max-loads 200000 --num-cpus 4 --snoop-merge

# Venus RTL, 2 cores
./build/RISCV_CCHI_RTL/gem5.opt configs/example/cchi_memtest.py \
    --max-loads 200000 --num-cpus 2 --downstream rtl --snoop-merge
```

Options: `--max-loads N` (0 = forever), `--num-cpus N`, `--downstream
earth|rtl`, `--snoop-merge` (exact multicore data; **required for Venus** —
without it Venus's victim back-invalidation drops dirty data), `--uncacheable
PCT`, `--functional PCT` (keep 0: functional side-channel writes bypass the
monitor), `--interval N`.

A clean run exits 0 with `monitorMismatches=0` in stats.

### Full workloads (kmhv3)

```bash
RTR=/mnt/c/OneDrive/ProjectFiles/XiangshanProjects/ready-to-run
export GCBV_REF_SO=$RTR/riscv64-nemu-interpreter-so

# Baseline
./build/RISCV/gem5.opt --outdir=m5out_coremark_base \
    configs/example/kmhv3.py --raw-cpt --generic-rv-cpt=$RTR/coremark-2-iteration.bin

# Earth
./build/RISCV_CCHI/gem5.opt --outdir=m5out_coremark_earth \
    configs/example/kmhv3.py --cchi \
    --raw-cpt --generic-rv-cpt=$RTR/coremark-2-iteration.bin

# Venus RTL
./build/RISCV_CCHI_RTL/gem5.opt --outdir=m5out_coremark_rtl \
    configs/example/kmhv3.py --cchi --cchi-downstream=rtl \
    --raw-cpt --generic-rv-cpt=$RTR/coremark-2-iteration.bin

# Linux boot (difftest OFF — see caveat below)
./build/RISCV_CCHI_RTL/gem5.opt --outdir=m5out_linux_rtl \
    configs/example/kmhv3.py --cchi --cchi-downstream=rtl --disable-difftest \
    --raw-cpt --generic-rv-cpt=$RTR/linux.bin
```

- `--cchi` replaces the tol2bus/L2/L3 hierarchy with the fabric (the classic
  L1s, their prefetchers, and the L1 performance model are unchanged; the L2
  prefetch engine is hosted on the agent and its emissions are stashed for
  future `DoPrefetchLoad/DoPrefetchStore` work — currently unimplemented by
  design). In the kmhv3 path `snoop_merge` is forced on (required by Venus).
- `--raw-cpt` is for raw `.bin` images; checkpoint slices (`.zstd` GCPT) go
  without it.
- Difftest caveat: with difftest ON, `linux.bin` diverges on **all** configs
  (baseline included) at the same pre-existing NEMU/gem5 interrupt-timing
  point — not a CCHI issue. Use `--disable-difftest` for linux; coremark runs
  clean with difftest ON.

### Reference results (validated)

| Workload | Baseline RISCV | Earth | Venus RTL |
|---|---|---|---|
| coremark (difftest on) | IPC 0.7857 | IPC 0.8128 | IPC 0.8105 |
| linux boot (difftest off) | 14,159,157 cyc | exit @ 5.104 G tick | exit @ 5.273 G tick |
| memtest 200k loads | — | 1/2/4c clean | 1/2c clean |

All coremark runs commit the identical 663,614 instructions; linux runs reach
`m5_exit` with 20,000,001–20,000,002 instructions (±1 is exit-boundary
interrupt timing, not corruption).

## Debug knobs

- `--debug-flags=CCHI` / `--debug-flags=Earth` — agent/fabric and Earth-home
  traces.
- `CCHI_RTL_TRACE=/path/dump.vcd` — waveform dump of the verilated top (VCD
  via `VerilatedVcdC`).
- `CCHI_RTL_TRACE_START=<gem5 tick>` — deferred trace open: the dump starts
  on the first fabric tick at/after this tick, keeping long runs from
  producing multi-GB waves before the region of interest.
- Fabric params (`CCHIFabric.py`): `monitor_enable`, `monitor_fail_on_mismatch`
  (data mismatch -> gem5 fatal), `monitor_trace` (very verbose),
  `earth_latency_rsp/dat`.
- Agent params (`CCHIL1Agent.py`): `snoop_merge`, `hit_latency`,
  `xaction_limit_{req,evt,snp}`.

## XSCache downstream (Oceanus L2 + OpenLLC + OpenNCB)

The `TestTop_L2OpenLLC` known top verilates the XSCache Oceanus L2 with
OpenLLC + OpenNCB (CHI-only cache subsystem: CoupledL2 tl2chi + OpenLLC,
AXI4 to memory) as the CCHI downstream. Prerequisite in the XSCache
checkout (generates `build/l2openllc/`, ~170 firtool files):

```bash
make init && make compile && make test-top-l2openllc
```

Build/run as shown above (dedicated `RISCV_CCHI_XSCACHE` variant); the run
commands are those of the Venus RTL endpoint (`--cchi
--cchi-downstream=rtl` for kmhv3, `--downstream rtl` for memtest) **plus
`--no-cchi-l2-pf`**: the XSCache RTL does not properly support the CCHI
stash transactions emitted by the hosted L2 prefetch engine, so all L2
prefetch must be disabled at run time with this downstream (L1 prefetch is
unaffected).

The fastest way to a linux boot is the wrapper script (checks
prerequisites, builds the variant on first run, then boots):

```bash
# required paths: CLI flags or the same-named environment variables
# (XSCACHE_DIR / CHIRON_DIR / READY_TO_RUN); there are no defaults
util/cchi/run_xscache_linux.sh \
    --xscache-dir=/path/to/XSCache \
    --chiron-dir=/path/to/CHIron \
    --ready-to-run=/path/to/ready-to-run            # build if needed + boot
util/cchi/run_xscache_linux.sh ... --flit-trace     # + CCHI flit-level log
util/cchi/run_xscache_linux.sh ... --vcd=/tmp/xscache.vcd --vcd-start=2680000000
```

Paths come from the `--xscache-dir` / `--chiron-dir` / `--ready-to-run`
flags (or the `XSCACHE_DIR` / `CHIRON_DIR` / `READY_TO_RUN` environment
variables); `GCBV_REF_SO` and `LINUX_BIN` default to files under
`READY_TO_RUN`, and `OUTDIR` defaults to `<repo>/m5out_linux_xscache`.
Missing required paths abort with an explicit error; see the script header
for the full option list. kmhv3 runs can also attach the CHIron flit
logger with `--cchi-flit-trace` (memtest: `--flit-trace`).

Constraints of the current generated top:

- One Type-1 port (`cchi_t1p0`, upstream NID hardcoded 0): single upstream
  node only. The two `cchi_t4p{0,1}` ports (req+dat only, no CHIron
  binding) and the `log_dump`/`log_clean` inputs are left tied off —
  matching XSCache's own cohestra smoke harness.
- The AXI master (`axi_m0`) has a 32-bit address: the fabric memory window
  must stay below 4GB. The default CCHI window `[0x8000_0000,
  0xA000_0000)` complies (and matches XSCache's own smoke configuration).
- First configure takes minutes (firtool file count) and a few GB in the
  temp dir. Regenerating the RTL in XSCache is picked up automatically
  (source mtimes/sizes are folded into the verilation stamp).

## User-specified RTL

Any Verilator-compatible home RTL can serve as the downstream endpoint:

```bash
.venv/bin/scons build/RISCV_CCHI_RTL/gem5.opt --gold-linker -j$(nproc) \
    WITH_CCHI=True WITH_CCHI_RTL=True \
    CCHI_RTL_TOP=my_home_top \
    CCHI_RTL_SRCS="/path/a.sv /path/b.sv /path/my_home_top.sv"   # dependency order
```

then run with `--cchi --cchi-downstream=rtl` (kmhv3) or `--downstream rtl`
(memtest). The generated model is bound through CHIron's
`V3CCHIInterface` / `V3AXISlaveInterface`, which detect ports **by name** at
compile time (C++ concepts — missing ports simply don't get bound), so the
top must follow the cohestra_v3 pin contract:

- **Clock/reset**: `clock`, `reset` (high-active, held for 100 cycles, then
  released with all pins quiesced by the interface).
- **CCHI Type-1 port sets**, one per upstream node, `N` = 0..7:
  `cchi_t1p<N>_{rxevt,rxreq}_{ready,valid,bits_*}` (upstream -> home),
  `cchi_t1p<N>_{txsnp,txrsp,txdat,rxrsp,rxdat}_{ready,valid,bits_*}`
  (home -> upstream). Flit fields follow CHIron's Type-1 layout
  (`bits_TxnID/SrcID/TgtID/Opcode/Addr/NS/...`, 64-bit `Addr`, 256-bit DAT
  `Data[0..]`). Optional upstream-width extensions
  `..._bits_WayValid` / `..._bits_Way` are picked up when present.
  `upstream_node_count` must not exceed the number of detected Type-1 ports.
- **AXI4 master ports** for memory traffic: `axi_m<M>_{aw,w,b,ar,r}*`,
  served by `CCHIAxiMemBridge` into the gem5 membus. B/R response FIFOs are
  sized 2 per port by the fabric.

Reference implementations in the CHIron checkout:
`cchi/cohestra/cohestra_v3/top_dummy.sv` (minimal loopback, good smoke test),
`cchi/cohestra/cohestra_v3/venus/` + `tb/venus_v3_top_extmem.sv` (full Venus
home; the `extmem` variant exposes the AXI masters on the boundary — this is
the one gem5 uses).

Caveats for new tops:

- AXI read data is expected critical-word-first (32B beats,
  `rd_start = addr[5]`); the bridge currently serves line-base-sequential
  data. This passes all data checking but is an assumption to revisit.
- Venus's victim flow requires `snoop_merge` (see above); assume other homes
  with back-invalidation do too.
- The fabric runs one RTL cycle per fabric clock tick; in kmhv3 runs the
  fabric sits on the CPU clock domain.

## Layout

- `cchi_l1_agent.{hh,cc}` — per-core bridge: gem5 L1 <-> Taurus Do* calls,
  home-snoop reflection into L1, grant-time fill snapshots.
- `cchi_fabric.{hh,cc}` — `CCHIFabric`: embedded `Cohestra::Instance`, Earth
  endpoint, RTL endpoint (`RtlStack`), snoop gate (snoop_merge), monitor,
  waveform tracing.
- `cchi_axi_mem_bridge.{hh,cc}` — AXI4-slave -> gem5 membus bridge (RTL
  builds only).
- `earth/` — vendored Earth behavioral home sources.
- `SConsopts` / `SConscript` — the `WITH_CCHI` / `WITH_CCHI_RTL` build
  plumbing described above.
