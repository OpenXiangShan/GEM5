# Upperbound Report: /tmp/debug/tage-new6

## What This Report Measures

- This is an *offline separability upper bound* computed from `bp.db`.
- For each chosen feature key (e.g., `(startPC, history)`), we compute the best possible
  accuracy under 0/1 loss by always predicting the *most frequent label* for that key
  (majority vote). This is Bayes-optimal given only that key.
- It is **NOT** an oracle that peeks at the future; it quantifies whether the available
  features contain enough information to separate patterns.

### Exit-slot (per-block) label

- Uses `TAGEMISSTRACE.realEnc` (0..32) as the true label for Exit-Slot multi-class classification.
- `UB_exit(startPC,hist)`: key is `(startPC, indexFoldedHist)`.
- `UB_exit(startPC,H)`: key is `(startPC, history_string)` (low 50 bits in current logging).

### Direction (per-branch) label

- Uses `TAGEMISSTRACE.actualTaken` (0/1) as the true label for direction prediction.
- `acc_dir(ref)`: measured accuracy `predTaken==actualTaken` in ref trace (if `predTaken` exists).
- `UB_dir(ref startPC,slot,hist)`: key is `(startPC, slot, indexFoldedHist)`, where
  `slot = ((branchPC - startPC) >> 1) & 31` approximates in-block position identity.
- `UB_dir(ref startPC,slot,H)`: key is `(startPC, slot, history_string)`.

### About `n/a`

- `n/a` means the db does not have usable samples for that metric (missing table/columns,
  or `TAGEMISSTRACE` exists but has 0 rows for that run).

| bench | BP mispred opt | BP mispred ref | delta | n_exit(opt) | acc_exit(opt) | UB_exit(startPC,hist) | UB_exit(startPC,H) | n_dir(ref) | acc_dir(ref) | UB_dir(ref startPC,slot,hist) | UB_dir(ref startPC,slot,H) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2fetch |  0.01% |  0.01% | +0.00% | 20.0k |  99.9% | 100.0% | 100.0% | n/a | n/a | n/a | n/a |
| 2fetch_self |  0.02% |  0.02% | +0.00% | 10.0k | 100.0% | 100.0% | 100.0% | n/a | n/a | n/a | n/a |
| alias_branches |  0.37% |  0.31% | +0.06% | 135.7k |  99.6% |  98.7% |  99.7% | 90.9k |  99.6% |  99.4% | 100.0% |
| aliasing_pattern_test |  3.71% |  0.76% | +2.95% | 3.1k |  96.7% |  97.1% |  97.4% | 983 |  98.5% |  98.4% | 100.0% |
| all_patterns_test |  3.27% |  0.75% | +2.52% | 38.3k |  97.3% |  96.2% |  96.5% | 8.8k |  98.0% |  98.7% |  99.9% |
| alternating_test |  0.36% |  0.28% | +0.08% | 2.5k |  99.6% |  99.8% |  99.8% | 997 |  99.7% | 100.0% | 100.0% |
| aluwidth |  0.96% |  0.96% | +0.00% | 209 |  99.0% |  99.0% |  99.0% | n/a | n/a | n/a | n/a |
| always_taken_test |  0.07% |  0.07% | +0.00% | 3.0k |  99.9% |  99.9% |  99.9% | n/a | n/a | n/a | n/a |
| bias_critical |  2.21% |  0.79% | +1.43% | 57.8k |  97.0% |  97.4% |  97.6% | 59.7k |  99.1% |  98.4% |  99.1% |
| brnum |  1.71% |  1.67% | +0.03% | 1.8k |  94.7% |  98.6% |  99.2% | 1.3k | 100.0% | 100.0% | 100.0% |
| brnum2 |  0.71% |  0.71% | +0.00% | 1.4k |  96.2% |  98.8% |  99.9% | 959 | 100.0% | 100.0% | 100.0% |
| brnum2_uftb |  0.25% |  0.23% | +0.02% | 14.1k |  99.2% |  99.8% | 100.0% | 9.4k | 100.0% | 100.0% | 100.0% |
| brnum3 |  0.43% |  0.43% | +0.00% | 3.2k |  98.0% |  99.5% |  99.9% | 960 | 100.0% | 100.0% | 100.0% |
| brsimple |  1.85% |  1.85% | +0.00% | 109 |  98.2% |  98.2% |  98.2% | n/a | n/a | n/a | n/a |
| brwidth |  0.02% |  0.02% | +0.00% | 217 |  99.1% |  99.1% |  99.1% | n/a | n/a | n/a | n/a |
| call_branch |  0.92% |  0.66% | +0.26% | 5.0k |  98.6% |  99.4% |  99.7% | 2.2k |  98.5% |  99.2% | 100.0% |
| confidence_trap |  4.48% |  2.51% | +1.97% | 4.6k |  94.3% |  96.2% |  97.7% | 3.8k |  97.0% |  92.2% |  99.4% |
| coremark10 |  5.54% |  3.62% | +1.92% | 599.2k |  92.7% |  93.9% |  97.1% | 551.7k |  95.3% |  96.1% |  99.1% |
| early_exits_test |  0.38% |  0.38% | +0.00% | 1.0k |  99.6% |  99.7% |  99.7% | 11 | 100.0% | 100.0% | 100.0% |
| fetchfrag |  0.75% |  0.75% | +0.00% | 30.2k |  99.0% | 100.0% | 100.0% | n/a | n/a | n/a | n/a |
| forloop |  0.72% |  0.33% | +0.39% | 10.4k |  99.1% |  99.6% |  98.2% | 11.1k |  99.7% |  99.8% |  99.0% |
| fpuwidth |  1.85% |  1.85% | +0.00% | 109 |  98.2% |  98.2% |  98.2% | n/a | n/a | n/a | n/a |
| gradual_transition_test |  0.12% |  0.12% | +0.00% | 2.5k |  99.9% |  99.9% |  80.1% | n/a | n/a | n/a | n/a |
| ifuwidth |  1.85% |  1.85% | +0.00% | 109 |  98.2% |  98.2% |  98.2% | n/a | n/a | n/a | n/a |
| imli_fixed_pos |  1.57% |  0.01% | +1.56% | 243.9k |  98.4% |  98.4% |  98.4% | 247.9k | 100.0% | 100.0% | 100.0% |
| imli_iter |  5.64% |  3.27% | +2.37% | 12.3k |  91.3% |  94.2% |  95.0% | 15.6k |  97.1% |  97.8% |  99.8% |
| imli_phase_shift |  1.51% |  0.01% | +1.50% | 517.9k |  98.5% |  98.5% |  98.5% | 511.9k | 100.0% | 100.0% |  99.2% |
| imli_threshold |  3.04% |  1.54% | +1.50% | 164.0k |  95.1% | 100.0% |  95.1% | 230.6k | 100.0% | 100.0% |  98.4% |
| indirect_branch |  0.07% |  0.07% | +0.00% | 3.0k |  99.5% | 100.0% | 100.0% | n/a | n/a | n/a | n/a |
| indirect_branch_alternating |  0.66% |  0.73% | -0.07% | 3.0k |  99.8% | 100.0% | 100.0% | n/a | n/a | n/a | n/a |
| indirect_branch_drift |  0.15% |  0.15% | +0.00% | 3.6k |  99.5% |  86.3% |  99.9% | 499 | 100.0% | 100.0% | 100.0% |
| indirect_branch_multi |  5.35% |  6.25% | -0.90% | 3.4k |  99.8% | 100.0% | 100.0% | n/a | n/a | n/a | n/a |
| jump_branch |  0.50% | 25.05% | -24.55% | 2.3k |  99.1% |  99.6% |  99.8% | 2.2k |  55.6% |  77.8% | 100.0% |
| local_mix | 17.63% |  4.64% | +12.99% | 80.7k |  83.2% |  86.0% |  89.2% | 58.0k |  95.1% |  96.1% |  97.6% |
| local_periodic |  0.61% |  0.21% | +0.40% | 18.6k |  99.2% |  99.5% |  99.4% | 13.8k |  99.7% |  99.8% | 100.0% |
| long_period_flip |  6.37% |  3.08% | +3.29% | 61.7k |  92.3% |  93.1% |  97.1% | 36.5k |  95.7% |  94.1% |  99.1% |
| majority_vote |  8.48% |  4.04% | +4.45% | 127.8k |  89.4% |  90.9% |  91.0% | 110.9k |  95.7% |  96.5% |  96.1% |
| multi_dim_pattern |  2.22% |  0.32% | +1.90% | 37.9k |  97.6% |  97.7% |  97.8% | 30.0k |  99.6% |  99.7% | 100.0% |
| nested_branches_test |  6.25% |  2.68% | +3.58% | 5.5k |  95.9% |  94.8% |  96.0% | 2.0k |  95.7% |  97.1% | 100.0% |
| never_taken_test |  0.15% |  0.15% | +0.00% | 2.0k |  99.9% |  99.9% |  99.9% | n/a | n/a | n/a | n/a |
| path_history |  1.54% |  0.09% | +1.45% | 7.8k |  98.4% |  98.4% |  97.0% | 4.0k |  99.9% | 100.0% | 100.0% |
| path_signature |  7.12% |  7.18% | -0.06% | 40.5k |  99.8% | 100.0% | 100.0% | 6.0k |  99.6% |  99.7% | 100.0% |
| prime_based_pattern_test |  6.43% |  0.83% | +5.60% | 4.3k |  96.2% |  96.8% |  96.4% | 1.0k |  98.3% |  94.1% | 100.0% |
| rare_branches_test |  0.99% |  0.64% | +0.35% | 2.1k |  99.0% |  99.0% |  98.8% | 902 |  99.0% |  99.2% |  99.0% |
| ras_recursive |  2.22% |  2.22% | +0.00% | 43 |  97.7% |  97.7% |  97.7% | n/a | n/a | n/a | n/a |
| rastest |  0.65% |  0.65% | +0.00% | 309 |  97.1% |  99.7% |  99.7% | n/a | n/a | n/a | n/a |
| renamewidth |  0.39% |  0.39% | +0.00% | 509 |  99.6% |  99.6% |  99.6% | n/a | n/a | n/a | n/a |
| resolve |  2.58% |  2.26% | +0.32% | 316 |  97.5% |  98.4% |  98.4% | 100 |  97.0% | 100.0% | 100.0% |
| return_branch |  0.45% |  0.38% | +0.07% | 4.8k |  99.1% |  99.7% |  99.7% | 2.2k |  99.4% |  99.9% | 100.0% |
| switching_pattern_test |  4.90% |  0.80% | +4.10% | 5.4k |  96.7% |  96.8% |  96.8% | 963 |  97.6% |  98.5% | 100.0% |
| tage1 |  0.49% |  9.67% | -9.18% | 3.6k |  98.5% |  99.9% |  99.9% | 10.0k |  89.5% | 100.0% | 100.0% |
| tage2 |  0.64% |  0.87% | -0.23% | 1.8k |  96.1% |  98.5% |  97.0% | 2.7k |  97.2% |  99.7% |  98.8% |
| tage3 |  0.40% |  0.35% | +0.05% | 1.5k |  99.5% |  99.6% |  99.7% | 998 |  99.7% | 100.0% | 100.0% |
| tage4 |  0.45% |  0.35% | +0.10% | 1.5k |  99.4% |  99.8% |  99.8% | 997 |  99.7% | 100.0% | 100.0% |
| tage5 | 14.29% | 14.29% | +0.00% | 52 |  82.7% |  80.8% |  92.3% | 3 | 100.0% | 100.0% | 100.0% |
| tage_aliasing |  1.46% |  0.42% | +1.04% | 33.6k |  98.2% |  98.7% |  98.6% | 39.9k |  99.6% |  99.6% |  99.5% |
| test_stringlen_v1 |  0.98% |  0.62% | +0.36% | 18.3k |  99.4% |  99.6% |  99.9% | 12.1k |  99.3% |  97.9% |  99.9% |
| test_stringlen_v2 |  2.17% |  1.30% | +0.87% | 37.9k |  97.8% |  98.3% |  99.8% | 25.1k |  98.3% |  98.3% |  99.9% |
| test_stringlen_v3 |  4.61% |  1.73% | +2.88% | 26.4k |  95.6% |  95.9% |  96.5% | 14.4k |  97.7% |  98.0% |  99.1% |
| three_bit_pattern_test |  6.93% |  0.52% | +6.41% | 4.4k |  96.0% |  96.2% |  96.1% | 993 |  99.0% |  99.5% | 100.0% |
| two_bit_pattern_test | 10.04% |  0.36% | +9.68% | 4.0k |  93.7% |  94.0% |  93.7% | 995 |  99.5% |  99.7% | 100.0% |
| weak_correlation | 17.55% | 12.96% | +4.59% | 72.3k |  88.6% |  86.3% |  94.2% | 48.1k |  88.0% |  89.7% |  99.8% |
| xor_dependency | 15.31% |  0.22% | +15.09% | 41.2k |  90.0% |  89.3% |  90.1% | 18.0k |  99.7% |  99.8% | 100.0% |

## Biggest BP mispred regressions (opt - ref)
- xor_dependency: +15.09%
- local_mix: +12.99%
- two_bit_pattern_test: +9.68%
- three_bit_pattern_test: +6.41%
- prime_based_pattern_test: +5.60%
- weak_correlation: +4.59%
- majority_vote: +4.45%
- switching_pattern_test: +4.10%
- nested_branches_test: +3.58%
- long_period_flip: +3.29%
