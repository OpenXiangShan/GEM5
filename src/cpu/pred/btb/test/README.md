# Branch Prediction Tests

Build these targets with `--unit-test`. Component tests use the production
component sources with unit-test support, except `uras.test`, which tests a
standalone `MockuRAS` and does not compile production `uras.cc`.

| Target | Tests | Scope |
|---|---:|---|
| `tage.test.debug` | 34 | TAGE, exact-history upper bound, prediction/history helpers |
| `btb.test.debug` | 36 | MBTB storage, prediction, training and victim cache |
| `abtb.test.debug` | 6 | AheadBTB prediction, training and thread isolation |
| `microtage.test.debug` | 8 | MicroTAGE prediction and teacher/resolved updates |
| `mgsc.test.debug` | 14 | MGSC selection, training and history |
| `ras_pop_push.test.debug` | 4 | Production RAS push/pop and recovery |
| `uras.test.debug` | 13 | MockuRAS stack, prediction and recovery |

## Prediction and Training

`prediction_helpers.hh` provides `predictForTest()` overloads that compose
component results into stages. These helpers are test code; they do not directly
execute production `DecoupledBPUWithBTB::predictComponents()`.

For an initialized TAGE test fixture:

```cpp
#include "cpu/pred/btb/test/prediction_helpers.hh"

std::vector<FullBTBPrediction> stagePreds(4);
boost::dynamic_bitset<> history(64, 0);
stagePreds[tage->getDelay()].btbEntries = {entry};
predictForTest(tage, startPC, history, stagePreds);
auto meta = tage->getPredictionMeta(0);
```

BTB component tests use the same helper:

```cpp
predictForTest(mbtb, startPC, history, stagePreds);
auto meta = mbtb->getPredictionMeta(0);
```

Keep the prediction metadata in the FetchTarget's component slot, then train
with `PredictionUpdateContext` and `PreparedUpdate`. See `predictUpdateCycle()`
in `btb.test.cc` and `btb_tage.test.cc` for complete examples. History recovery
uses `HistoryRecoveryContext`; it does not use the old stream-based interface.

Candidates expose `taken`, `baseTaken` and `predictedTarget`. Exact BTB counter
assertions should read the storage snapshot through `predictionStorageEntries()`;
checking direction alone cannot verify counter strength or unchanged entries.

## Build and Run

```bash
scons build/RISCV/cpu/pred/btb/test/tage.test.debug \
  build/RISCV/cpu/pred/btb/test/btb.test.debug \
  build/RISCV/cpu/pred/btb/test/abtb.test.debug \
  build/RISCV/cpu/pred/btb/test/microtage.test.debug \
  build/RISCV/cpu/pred/btb/test/mgsc.test.debug \
  build/RISCV/cpu/pred/btb/test/ras_pop_push.test.debug \
  build/RISCV/cpu/pred/btb/test/uras.test.debug --unit-test -j24

for test in tage btb abtb microtage mgsc ras_pop_push uras; do
  ./build/RISCV/cpu/pred/btb/test/$test.test.debug
done
```

Do not run unit-test and production SCons builds concurrently in the same build
directory. A focused test can be selected with, for example,
`--gtest_filter=BTBTAGETest.BasicPrediction`.
