# perlbench_checkspam_5886：正式 TEA baseline

baseline validation.json 已通过：20M warmup 后 19,999,994 committed instructions、4,102,519 cycles，选定计数与正式 CI 末块逐值一致，difftest 和 TEA 守恒通过。TEA period=1；本轮 PC 的总权重包括 Computing，PC×状态交叉不可得，PSV 事件可重叠，不能把总权重或事件之和当可消除 stall。

全局 TEA CPI：Computing 0.147203、Stalled 0.047534、Flushed 0.009725、Drained 0.000664；未归因 unresolved 0.001026。事件 CPI（可重叠）：ST_L1 0.012058、ST_LLC 0.011239、ST_TLB 0.007276、FL_MB 0.009020；不是可直接相加的互斥周期。

| 新 ROI 总权重排名 | PC / 实际 ELF 符号和指令 | 总权重 CPI | 可观察事件 CPI |
|---|---|---:|---|
| 1 | `0x92e18` S_regmatch `lw a5,0(a5)` | 0.013938 | ST_L1 约 0.0000009 |
| 2 | `0x92f36` S_regmatch `lbu a2,1(a4)` | 0.004732 | 见 JSON |
| 3 | `0x96d10` S_find_byclass.isra.0 `bgeu s2,a3,96cee` | 0.004402 | FL_MB 0.003844 |
| 4 | `0x9bafa` Perl_runops_standard `ld a5,16(a5)` | 0.003641 | ST_L1 0.003009、ST_LLC 0.002706、ST_TLB 0.001189，可重叠 |
| 5 | `0x92dfc` S_regmatch `sub a4,s5,s2` | 0.002273 | 见 JSON |

先前候选 `0x92e18`、`0x96d10` 由新 ROI 数据分别命中第 1、3 位；`0x92e1a` 为 0.001369 CPI，不在前五。事件排名独立看：FL_MB 首位 0x96d10，ST_L1 / ST_LLC 首位 0x9bafa；事件关联不证明 PC 的独立因果，也不可把 ST_L1 与 ST_LLC 相加。

ELF 是 profile 的 `elf/perlbench_checkspam.elf`，SHA256 `df8fd054a6a9c78b6058a8af595c06e6c1b654867a9c59b0a1642b3a4346721d`，其与 profile rootfs 内 perlbench 哈希相同。函数符号和指令由此 ELF objdump 直接确认；addr2line 对这些 PC 均无源码行。当前可读实际构建树中函数级语义候选分别是 `regexec.c:2296`（S_regmatch）、`regexec.c:961`（S_find_byclass）、`run.c:35`（Perl_runops_standard）、`hv.c:383`（S_hv_fetch_common）；当前源码与 8 月 profile 的精确历史版本尚未证明，不能将 PC 精确归到这些行。

当前只见 baseline validation，candidate 尚无 validation，故未比较配对事件或净收益。完整PC排名、原始规范化值、ELF/CSV哈希和守恒信息见同名 JSON。
