# Astra B 轮定向复核（受主审提示，独立原文 A 保留不变）

版本：base `e34d8f6e7901b34fbd6111cfa5615136aaeefaf1`；2026-10-28 当前 working-tree，复核 `TLB::translateFunctional` 新检查与 `tests/test-progs/riscv-upperbound-fetch/fetch.S`。只读源码和已有 ELF/bin；本轮未编译或仿真，未改源码。

## B1 — A1 修复静态通过，整机结果待验证

- 位置：`src/arch/riscv/tlb.cc:3064-3075`。
- `configFunctional` 先确定 translation state，随后按单阶段 satp / 两阶段 vsatp 选择模式；仅 Execute 且有效 Sv39/Sv48 模式检查符号扩展，返回原 `vaddr` 的 `INST_PAGE` fault，发生在 walker 舍弃高 VPN 位之前。
- 对 A1 的 S/U + Sv39/Sv48 单阶段触发，修复阻断了原关键链；M-mode/Bare 不被新检查误拒绝。未改变 timing 路径。
- 状态：A1 源码修复成立，未发现新增确定缺陷。两阶段选择逻辑有源码依据，但未做 H-extension 整机验证，不能把此次结论扩大为全部 H 模式通过。

## B2 — 新测试能区分 A1；必要运行结果仍待提供

- 位置：`fetch.S:38-55,85-110,120-128`。
- Sv39 `0x8000001000` 和 Sv48 `0x1000000001000` 都是非 canonical 地址，其各级 VPN 与合法 X-only `0x1000` 相同；`jr s5` 是实际架构跳转，不是仅检查数据地址。handler 验证 cause=12、mtval=非法地址、mepc=非法地址。
- 原错误实现若执行别名代码，将增加 s1 并沿遗留 ra 返回 loop，不会到 success；结合 maxinsts 和只接受 m5_exit 的判据，可以区分失败。不要把 reaching maxinsts 当通过。
- NX PTE `0xc7` 无 X；X-only PTE `0x49` 有 V/X/A、无 R；页末 RVC `0x8082` 和 32b 前半 `0x0013` 对应预期分类。跨页 fault 检查 `mepc=0x5ffe, mtval=0x6000`，能区分 instruction PC 与失败字节。
- 实际验证：反汇编两个已有 ELF，确认 load-immediate 和 jr；读取两个 28672-byte bin，确认 physical execute_page+4094 是 `82 80`、crossing_page+4094 是 `13 00`，页表区域初始全零。构造成立；尚未读取整机 log，不能宣称测试通过。
- 覆盖缺口：当前测试未强制制造“错路径 NX/unmapped fault 已生成，再被更老分支 squash”的情形；重复 call/ret 不能证明该特定边界。若要声称错路径 fault 恢复已验证，需要独立可控错预测用例与 retirement/陷阱证据。

## B3 — 统计最小修正建议（建模口径，非架构 bug）

- 当前 `src/cpu/o3/fetch.cc:1257` 在 `run_out` 后递增 `idealFetchTargetsUsed`，代表 FTQ target **完成消费**；`3398` 却采样到既有 `fetchTargetsStartedPerCycle`（其初始化上界还是 numThreads，单线程只覆盖 0/1，值 2 进入 overflow）。
- 新增 `statistics::Distribution idealFetchTargetsConsumedPerCycle`，描述为当前周期完成消费的 ideal-fetch FTQ targets，`.init(0, 2, 1)`。ideal 分支改为对此采样，不再把 consumed 值写入旧 started 直方图；原 timing 分支保持不变。
- 为保持旧 request-start 统计含义，ideal 分支可以对旧 `fetchTargetsStartedPerCycle` 和 `fetchLineRequestsCreatedPerCycle` 均采样 0，表示没有创建 timing 请求。也可不采样旧 target 分布，但报告必须明确 inactive；采样 0 更容易横向解释。
- 不需要为此改变 bandwidth 控制变量；如加总量 scalar，则在现有递增点同步加一。需注明 fault/false-hit squash 不计为 completed target，跨周期部分消费时可为 0，即使该周期已经供应指令。
- 吞吐证据同时报告 supplies/bytes、consumed 直方图、twoFetchSuccesses、idealSecondPredictionRequests/Enqueues；不要把 completed targets 等同于目标第一次访问次数。
