# SDBP row-hit ablation

Tested commit: `867213de6fb8fa00b004a6260dd2811565971948`.

The test starts from the `baserun.md` commit `de1df30a09b46efd7948d75de46fcedd085abb54` and changes only the DRAMsim3 row-buffer policy from `OPEN_PAGE` to `CLOSE_PAGE`.

## Configuration

- `configuration=idealkmhv3.py`
- `benchmark_type=spec06-rva23-novec-gcc16-1.0c`
- 1094 checkpoints, 29 benchmarks, 55 workloads
- LRU and SDBP each run with prefetch on and off
- SDBP sampler: 128 sampled sets, 6 ways
- SDBP defaults retained: L1-copy protection on, dead-first on, youngest protection 0, bypass off
- DRAMsim3: XiangShan DDR4-3200 2-channel config with `row_buf_policy=CLOSE_PAGE`

The close-page ini is [xiangshan_DDR4_8Gb_x8_3200_2ch_close_page.ini](../../ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch_close_page.ini). Relative paths fail because the CI runner changes into the archive directory; the dispatched argument therefore uses `$GEM5_HOME`, which the workflow expands before launch.

## Results

| Configuration | Open-page score | Close-page score | Change |
| --- | ---: | ---: | ---: |
| No prefetch, LRU | 46.77618497 | 45.47083705 | -2.79% |
| No prefetch, SDBP | 47.10917478 | 45.79254328 | -2.80% |
| Prefetch on, LRU | 66.28873291 | 64.49838153 | -2.70% |
| Prefetch on, SDBP | 66.17010717 | 64.48787226 | -2.54% |

| Comparison | Open-page | Close-page | Shift |
| --- | ---: | ---: | ---: |
| SDBP / LRU, no prefetch | +0.71188% | +0.70750% | -0.00438 pp |
| SDBP / LRU, prefetch on | -0.17895% | -0.01629% | +0.16266 pp |

Closing row hits removes most of the prefetch-on SDBP deficit, but does not make SDBP faster than LRU. With prefetch off, the SDBP advantage is essentially unchanged.

Benchmark changes explain why the aggregate shifts differ:

- With prefetch on, milc moves from `-2.274%` for SDBP/LRU to `+0.114%`; gcc moves from `-0.359%` to `+0.347%`, and bwaves from `-0.096%` to `+0.866%`.
- With prefetch off, milc moves from `-0.695%` to `+0.210%`, but lbm moves from `+0.686%` to `-0.711%`, largely canceling the aggregate change.
- Large no-prefetch SDBP gains on libquantum (`+6.36%`), xalancbmk (`+5.51%`), mcf (`+3.76%`), and sphinx3 (`+3.03%`) remain in close-page mode.

## Validation

All four runs completed with `1094/1094` points and zero aborts. Every generated point config uses the close-page ini and the expected replacement policy. Every SDBP point has sampler count 128, associativity 6, L1-copy protection enabled, youngest protection 0, and bypass disabled. Across all 4,376 point configurations, `num_read_row_hits` is zero.

## Remote runs

- No prefetch, LRU: [41989493742](https://github.com/OpenXiangShan/GEM5/actions/runs/41989493742)
- No prefetch, SDBP: [41989496943](https://github.com/OpenXiangShan/GEM5/actions/runs/41989496943)
- Prefetch on, LRU: [41989499034](https://github.com/OpenXiangShan/GEM5/actions/runs/41989499034)
- Prefetch on, SDBP: [41989501587](https://github.com/OpenXiangShan/GEM5/actions/runs/41989501587)

Detailed outputs: [verification.json](verification.json) and [benchmark-comparison.csv](benchmark-comparison.csv).
