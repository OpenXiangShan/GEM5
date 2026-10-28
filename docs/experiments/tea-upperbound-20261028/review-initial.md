# Astra 首轮独立 Review（保留原文）

版本：base/HEAD `e34d8f6e7901b34fbd6111cfa5615136aaeefaf1`；head 是 2026-10-28 当前 working-tree diff，分支 `codex/tea-upperbound-24base-20261028`。未接收其他助手候选，未修改源码/Git/配置，未运行整机构建或仿真。检查时 diff 为 9 文件，196 additions / 5 deletions。

## A1 — P2 confirmedbug：idealInstructionFetch 绕过非 canonical 指令地址检查

- 位置：新增 `src/cpu/o3/fetch.cc:3510-3511`；底层证据 `src/arch/riscv/tlb.cc:3044-3071`、`src/arch/riscv/pagetable_walker.cc:2067-2069`。正常路径检查在 `src/arch/riscv/tlb.cc:2463-2472`。
- 最小触发：单线程 FS、S/U 模式、Sv39，虚拟地址 `0x1000` 存在可执行映射且 PMP 允许；实际跳转到 `0x8000001000`（bit39=1，bit38=0，非 canonical）。Sv48 可构造相应上位地址。
- 影响：应产生 instruction page fault 且 tval 保存非法地址；functional walker 仅取各级 VPN 的低位，不检查符号扩展，因此沿与 `0x1000` 相同的页表路径取得代码并执行，可能导致 difftest 不一致。资源/时序理想化不允许改变这个架构结果。
- 关键链：`supplyIdealInstructionBytes -> MMU::translateFunctional(Execute) -> TLB::translateFunctional -> Walker::startFunctional -> setupWalk/stepWalk`。`checkPermissions` 只检查 R/W/X/U 等权限，不能补足 canonical 检查。
- base 对照与分类：普通 base Fetch 在约 894/923 行调用 `translateTiming`，会经 `doTranslate` 拒绝非法地址。底层 functional API 漏检是已有缺陷；本次把它用于架构指令供给，属于**新功能继承的缺陷，并相对原 timing Fetch 形成行为回归**，不能仅因底层旧代码已有而排除。
- 验证：完整静态调用链；只读 Python 验证 `0x1000` 与 `0x8000001000` 的 Sv39 VPN 均为 `(0,0,1)` 且页内偏移相同。尚未整机复现，Python 仅证明地址别名关键假设。
- 建议：为 functional Execute 翻译补齐有效翻译模式下的 canonical 检查，保留原请求地址生成 fault；加该实际跳转用例，并检查 mepc/sepc 和 mtval/stval。

## 首轮未发现确定问题的范围与必要验证缺口

- 串行第二次预测：第一项先保存独立 FTQ history/metas、推进 PC、RAS 和 folded histories 后入队，才再次 requestNewPrediction；TAGE/ITTAGE/MGSC/MicroTAGE/RAS 的 putPCHistory 分配新的 shared metadata。squash 按 FTQ target 恢复快照；PairTAGE 已禁止组合。本轮未发现快照复用或串行次序错误。
- 按需取指：RVC 仅取首半字；32b 后半字独立 Execute 翻译；fault carrier 使用当前 instruction PC，而页错使用失败半字地址。Execute 权限检查能区分 NX 与 X-only；fault carrier 带 localSquashVer，继续进入正常 squash 链。以上为源码结论，未做整机微测。
- 必要验证：页末 RVC + 下一页未映射、跨页 32b + 下一页未映射/NX、X-only 正常执行、实际/错路径非法目标、第二个 FTQ 中 call/return 后分支恢复；至少保留 fault PC/tval 与 squash 后 retirement 证据。需加入 A1 非 canonical 地址测试。
- 统计口径限制：`idealFetchTargetsUsed` 在 target 用尽时递增，但被采样到 `fetchTargetsStartedPerCycle`；跨周期部分消费的 target 会显示 0，不能单独以该直方图证明每周期供给目标数。第二次预测有独立 requests/enqueues 计数；现有 predictionsStartedPerCycle 文义是 distinct threads，保持 1 不证明串行第二次没工作。这是报告解释限制，本轮不列为功能 bug。
- 未深入：H 两阶段 functional walker、trace 与 ideal 参数混用、SE、后端调度实现/性能因果、全部组件训练与银行冲突模型、整机 difftest/SPEC 分数。S-mode SUM 对 U 页 Execute 的检查问题存在于共同旧 checkPermissions，属于纯基线问题，本轮不计 finding。
- `git diff --check` 通过。
