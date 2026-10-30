# BPU context flush model

The BTB-based Kunminghu frontend can model a cold branch predictor after a
successfully retired `fence.i`. The model is optional and disabled by default.

## Configuration

Run `configs/example/kmhv3.py` with `--enable-context-flush` to enable the model.
`--flush-latency` sets the recovery window in CPU cycles (default: 516).
The corresponding `DecoupledBPUWithBTB` parameters are `enableContextFlush`
and `flushLatency`.

The 516-cycle default is an aggregate modeling assumption. It does not model
the complete operating-system context-switch or cache-maintenance latency.

## Behavior and scope

The causal chain is:

`retired fence.i -> cold predictor/history state -> forced fall-through window
-> branch recovery and predictor relearning -> IPC and flush statistics`.

Each flush clears enabled predictor components, resets speculative histories,
and advances the context epoch. Components are shared, so all configured
threads enter the recovery window. Existing FTQ entries remain available for
in-flight instructions, but entries from an older epoch cannot train the cold
predictor or restore their old prediction history. A wrong-path `fence.i` does
not trigger a flush because the trigger runs at successful retirement.

During the window, predictions fall through and do not train the predictor.
Existing FTQ capacity, pipeline backpressure and branch recovery still apply.
The window is an aggregate availability model, not a modeled table-clear
pipeline. Normal prediction and training resume after the window expires.

The per-cycle window check is bounded by the configured thread count. Predictor
table scans occur only on flush events; history resets are bounded by the
configured predictor and thread sizes.

## Observability

Statistics under `system.cpu.branchPred` include:

- `bpuFlushes`: retired-instruction flush events.
- `bpuComponentFlushes`: flush dispatches to each enabled predictor component.
- `bpuFlushStallCycles`: sum of configured window lengths over flush events.
- `bpuFlushBubbles`: thread cycles observed inside a flush window.
- `bpuFlushUntakenPredictions`: forced fall-through predictions.

`bpuFlushStallCycles` is configured accounting; overlapping windows or early
simulation exit can make it differ from `bpuFlushBubbles`. The `DecoupleBP`
debug flag reports flush triggers and window end ticks.

## Validation

The standard unit-test target includes cold-state tests for MBTB, TAGE,
MicroTAGE, MGSC and RAS, plus tests that speculative resets preserve learned
predictor tables:

```sh
scons build/RISCV/unittests.opt -j64 --unit-test
```

Two small raw images exercise the retirement trigger and a wrong-path fence:

```sh
make -C util/bpu_flush all wrong-path
./build/RISCV/gem5.opt --outdir=m5out/fence-i \
  configs/example/kmhv3.py --raw-cpt \
  --generic-rv-cpt=util/bpu_flush/fence_i_raw.bin --enable-context-flush
./build/RISCV/gem5.opt --outdir=m5out/fence-i-wrong-path \
  configs/example/kmhv3.py --raw-cpt \
  --generic-rv-cpt=util/bpu_flush/fence_i_wrong_path.bin --enable-context-flush
```

Set `GCBV_REF_SO` to the pinned reference returned by
`python3 util/nemu_ref/resolve.py normal` before running the images.
The retired-fence image should report one flush; the wrong-path image should
report zero. With the feature disabled, both images should report zero flushes.
