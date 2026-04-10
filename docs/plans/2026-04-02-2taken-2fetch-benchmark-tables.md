# 2-Taken / 2-Fetch Benchmark Tables

## 中文版

### 1. 总体分数

| Version | SPECint Weighted Score | Delta vs Baseline | Delta vs Intermediate |
| --- | ---: | ---: | ---: |
| Baseline (`fc6422716`) | 20.6959 | - | - |
| Intermediate (`eac43be47`) | 21.4439 | +3.61% | - |
| Final (`1cae93a`) | 21.8804 | +5.72% | +2.04% |

### 2. 各 benchmark 分数对比

| Benchmark | Ref Score | Mid Score | Final Score | Final vs Ref | Final vs Mid |
| --- | ---: | ---: | ---: | ---: | ---: |
| perlbench | 16.94 | 18.31 | 19.97 | +17.90% | +9.05% |
| xalancbmk | 33.96 | 36.91 | 38.17 | +12.40% | +3.41% |
| sjeng | 13.17 | 14.47 | 14.51 | +10.20% | +0.30% |
| gcc | 21.17 | 22.61 | 23.24 | +9.77% | +2.80% |
| gobmk | 15.61 | 16.35 | 16.59 | +6.24% | +1.47% |
| h264ref | 25.23 | 25.43 | 26.29 | +4.18% | +3.36% |
| astar | 14.33 | 14.75 | 14.89 | +3.91% | +0.99% |
| bzip2 | 11.26 | 11.41 | 11.55 | +2.52% | +1.20% |
| libquantum | 46.86 | 46.79 | 47.58 | +1.53% | +1.69% |
| mcf | 34.65 | 34.93 | 34.93 | +0.80% | -0.01% |
| omnetpp | 21.85 | 21.91 | 22.00 | +0.72% | +0.43% |
| hmmer | 17.07 | 17.07 | 17.08 | +0.09% | +0.07% |

### 3. 高收益 benchmark 的关键证据

| Benchmark | Score Gain vs Ref | `fetch_nisn_mean` Gain | Final `doubleFetchCycle` Share | Final `2-Fetch` Success Rate | `frontendBound` Drop | `fetchBubbles` Drop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| perlbench | +17.90% | +33.83% | 29.96% | 33.30% | -89.77% | -89.98% |
| xalancbmk | +12.40% | +18.55% | 46.47% | 55.25% | -83.29% | -84.70% |
| sjeng | +10.20% | +33.15% | 21.75% | 30.48% | -59.45% | -63.19% |
| gcc | +9.77% | +22.07% | 42.76% | 49.00% | -73.46% | -78.06% |
| gobmk | +6.24% | +32.25% | 24.48% | 32.15% | -70.72% | -71.42% |
| h264ref | +4.18% | +5.74% | 11.60% | 17.35% | -89.74% | -90.43% |

### 4. Final 聚合 2-Fetch 计数器

| Metric | Value |
| --- | ---: |
| `twoFetchOpportunity` | 61.09M |
| `twoFetchTaken` | 22.98M |
| `2-Fetch success rate` | 37.61% |
| `doubleFetchCycle share` | 28.70% |

### 5. Final 失败原因拆分

| Failure Class | Share of Failed Opportunities |
| --- | ---: |
| Only `not predicted taken` | 29.69% |
| Only `no next stream` | 13.83% |
| Only `span too large` | 7.01% |
| Only `target not in buffer` | 10.51% |
| Both `span too large` and `target not in buffer` | 38.96% |

### 6. Intermediate vs Final: idealized fetch-window 效果

| Benchmark | Final vs Mid | `span_rate` Mid | `span_rate` Final | `oob_rate` Mid | `oob_rate` Final | `doubleFetchCycle` Mid | `doubleFetchCycle` Final | `fetchBubbles` Drop vs Mid |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| perlbench | +9.05% | 75.54% | 52.30% | 4.00% | 52.55% | 8.88% | 29.96% | -79.78% |
| xalancbmk | +3.41% | 37.12% | 27.14% | 3.43% | 23.97% | 33.34% | 46.47% | -56.24% |
| h264ref | +3.36% | 39.44% | 30.34% | 13.51% | 37.33% | 4.67% | 11.60% | -86.65% |
| gcc | +2.80% | 51.27% | 31.78% | 6.07% | 32.82% | 22.30% | 42.76% | -44.97% |
| libquantum | +1.69% | 57.10% | 28.58% | 8.63% | 8.60% | 25.60% | 56.79% | -99.86% |
| gobmk | +1.47% | 53.37% | 35.27% | 5.56% | 34.75% | 9.65% | 24.48% | -28.94% |
| bzip2 | +1.20% | 45.40% | 25.11% | 7.05% | 25.63% | 11.51% | 27.72% | -57.17% |
| astar | +0.99% | 73.01% | 25.10% | 1.12% | 26.46% | 1.49% | 31.15% | -25.33% |
| omnetpp | +0.43% | 69.10% | 43.04% | 4.33% | 36.64% | 4.19% | 23.70% | -66.27% |
| sjeng | +0.30% | 53.65% | 38.44% | 5.08% | 38.10% | 7.53% | 21.75% | -6.77% |
| hmmer | +0.07% | 20.08% | 17.35% | 1.36% | 18.43% | 9.95% | 11.80% | -25.89% |
| mcf | -0.01% | 27.53% | 10.92% | 31.53% | 29.38% | 8.89% | 26.13% | -35.61% |

### 7. 可直接贴 PPT 的短结论

- 总体收益: final vs baseline `+5.72%`，final vs intermediate `+2.04%`
- 最大受益 workload: `perlbench / xalancbmk / sjeng / gcc / gobmk`
- 核心证据: `fetch_nisn_mean` 上升、`fetchBubbles` 大降、`frontendBound` 大降、`doubleFetchCycle` 占比提高
- 主要瓶颈: `span too large` + `target not in buffer`，而不是 `no next stream`

---

## English Version

### 1. Overall Scores

| Version | SPECint Weighted Score | Delta vs Baseline | Delta vs Intermediate |
| --- | ---: | ---: | ---: |
| Baseline (`fc6422716`) | 20.6959 | - | - |
| Intermediate (`eac43be47`) | 21.4439 | +3.61% | - |
| Final (`1cae93a`) | 21.8804 | +5.72% | +2.04% |

### 2. Per-Benchmark Score Comparison

| Benchmark | Ref Score | Mid Score | Final Score | Final vs Ref | Final vs Mid |
| --- | ---: | ---: | ---: | ---: | ---: |
| perlbench | 16.94 | 18.31 | 19.97 | +17.90% | +9.05% |
| xalancbmk | 33.96 | 36.91 | 38.17 | +12.40% | +3.41% |
| sjeng | 13.17 | 14.47 | 14.51 | +10.20% | +0.30% |
| gcc | 21.17 | 22.61 | 23.24 | +9.77% | +2.80% |
| gobmk | 15.61 | 16.35 | 16.59 | +6.24% | +1.47% |
| h264ref | 25.23 | 25.43 | 26.29 | +4.18% | +3.36% |
| astar | 14.33 | 14.75 | 14.89 | +3.91% | +0.99% |
| bzip2 | 11.26 | 11.41 | 11.55 | +2.52% | +1.20% |
| libquantum | 46.86 | 46.79 | 47.58 | +1.53% | +1.69% |
| mcf | 34.65 | 34.93 | 34.93 | +0.80% | -0.01% |
| omnetpp | 21.85 | 21.91 | 22.00 | +0.72% | +0.43% |
| hmmer | 17.07 | 17.07 | 17.08 | +0.09% | +0.07% |

### 3. Key Evidence for High-Gain Benchmarks

| Benchmark | Score Gain vs Ref | `fetch_nisn_mean` Gain | Final `doubleFetchCycle` Share | Final `2-Fetch` Success Rate | `frontendBound` Drop | `fetchBubbles` Drop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| perlbench | +17.90% | +33.83% | 29.96% | 33.30% | -89.77% | -89.98% |
| xalancbmk | +12.40% | +18.55% | 46.47% | 55.25% | -83.29% | -84.70% |
| sjeng | +10.20% | +33.15% | 21.75% | 30.48% | -59.45% | -63.19% |
| gcc | +9.77% | +22.07% | 42.76% | 49.00% | -73.46% | -78.06% |
| gobmk | +6.24% | +32.25% | 24.48% | 32.15% | -70.72% | -71.42% |
| h264ref | +4.18% | +5.74% | 11.60% | 17.35% | -89.74% | -90.43% |

### 4. Final Aggregate 2-Fetch Counters

| Metric | Value |
| --- | ---: |
| `twoFetchOpportunity` | 61.09M |
| `twoFetchTaken` | 22.98M |
| `2-Fetch success rate` | 37.61% |
| `doubleFetchCycle share` | 28.70% |

### 5. Final Failure Breakdown

| Failure Class | Share of Failed Opportunities |
| --- | ---: |
| Only `not predicted taken` | 29.69% |
| Only `no next stream` | 13.83% |
| Only `span too large` | 7.01% |
| Only `target not in buffer` | 10.51% |
| Both `span too large` and `target not in buffer` | 38.96% |

### 6. Intermediate vs Final: Idealized Fetch-Window Effect

| Benchmark | Final vs Mid | `span_rate` Mid | `span_rate` Final | `oob_rate` Mid | `oob_rate` Final | `doubleFetchCycle` Mid | `doubleFetchCycle` Final | `fetchBubbles` Drop vs Mid |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| perlbench | +9.05% | 75.54% | 52.30% | 4.00% | 52.55% | 8.88% | 29.96% | -79.78% |
| xalancbmk | +3.41% | 37.12% | 27.14% | 3.43% | 23.97% | 33.34% | 46.47% | -56.24% |
| h264ref | +3.36% | 39.44% | 30.34% | 13.51% | 37.33% | 4.67% | 11.60% | -86.65% |
| gcc | +2.80% | 51.27% | 31.78% | 6.07% | 32.82% | 22.30% | 42.76% | -44.97% |
| libquantum | +1.69% | 57.10% | 28.58% | 8.63% | 8.60% | 25.60% | 56.79% | -99.86% |
| gobmk | +1.47% | 53.37% | 35.27% | 5.56% | 34.75% | 9.65% | 24.48% | -28.94% |
| bzip2 | +1.20% | 45.40% | 25.11% | 7.05% | 25.63% | 11.51% | 27.72% | -57.17% |
| astar | +0.99% | 73.01% | 25.10% | 1.12% | 26.46% | 1.49% | 31.15% | -25.33% |
| omnetpp | +0.43% | 69.10% | 43.04% | 4.33% | 36.64% | 4.19% | 23.70% | -66.27% |
| sjeng | +0.30% | 53.65% | 38.44% | 5.08% | 38.10% | 7.53% | 21.75% | -6.77% |
| hmmer | +0.07% | 20.08% | 17.35% | 1.36% | 18.43% | 9.95% | 11.80% | -25.89% |
| mcf | -0.01% | 27.53% | 10.92% | 31.53% | 29.38% | 8.89% | 26.13% | -35.61% |

### 7. PPT-Ready Takeaways

- Overall gain: final vs baseline `+5.72%`, final vs intermediate `+2.04%`
- Biggest winners: `perlbench / xalancbmk / sjeng / gcc / gobmk`
- Main evidence: higher `fetch_nisn_mean`, lower `fetchBubbles`, lower `frontendBound`, higher `doubleFetchCycle` share
- Main bottleneck: `span too large` + `target not in buffer`, not `no next stream`
