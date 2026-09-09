# GEM5 Difftest 的状态与事件边界

Difftest 在架构指令边界推进参考模型，并检查选定的架构状态。
它不是逐周期比较，也不保证每次提交都检查全部寄存器。

## 源码入口

| 文件 | 职责 |
| --- | --- |
| `src/cpu/base.cc` | 创建各 hart 的 REF，在 CPU startup 时捕获初态，接管 CPU 时移交 difftest 状态 |
| `src/cpu/difftest_cpu.cc` | DUT 状态采集、首次同步、REF 推进、比较、错误报告和共享内存差异处理 |
| `src/cpu/difftest.hh`、`difftest.cc` | NEMU/Spike ABI 缓冲区、动态符号加载和寄存器复制接口 |
| `src/cpu/o3/commit.cc` | 提供已提交结果，在异常改变 DUT 状态前建立 REF 初态 |
| `src/cpu/simple/base.cc` | SimpleCPU 的寄存器采集和提交入口 |

## 每个 hart 的状态

`DiffAllStates` 中的缓冲区各有用途：

- `initialDutState`：startup 捕获的 DUT 初态，后续比较不修改它。
- `initialStateCaptured`：初态是否已捕获，避免使用未初始化或遗漏的快照。
- `referenceInitialized`：内存和初始寄存器是否已同步到 REF。
- `referenceRegFile`：从 REF 读回的架构状态；允许的 skip/reconciliation 会修改它并写回 REF。
- `gem5RegFile`：用于比较与诊断的 DUT 工作缓冲区，并非每一步都完整刷新的快照。

GPR、FPR、vector 的读取由 CPU 实现；O3 使用 committed rename map，
SimpleCPU 使用指定 `tid` 的 ThreadContext。启动 CSR 的导出包括特权状态，
并区分原始 CSR 与合成值：`MIP/MIE` 来自中断控制器，`VCSR/VLENB`
使用 ISA 的架构读取接口，`FCSR` 由 `FFLAGS/FRM` 合成。
不能把这些读取统一替换成 `readMiscRegNoEffect()`。

## 首次同步

正常提交路径的时序是：

```text
startup：捕获 DUT S0
DUT：    S0 --指令 I--> S1
REF：    写入 S0 --指令 I--> R1
检查：   S1 与 R1 的选定状态
```

`ensure_difftest_reference(tid, event_pc)` 是幂等入口：首次调用复制内存和
保存的 S0 到 REF，读回 REF 的规范化状态，并标记初始化完成。
`event_pc` 必须对应首次事件之前的 PC，与保存的初态 PC 一致。

首次架构事件不一定是普通提交：

- 普通提交在 `difftestStep()` 中确保初始化。
- O3 架构异常在 DUT 执行 trap 之前确保初始化。
- O3 中断通过 `difftestRaiseIntr()`，在 REF 接收中断之前确保初始化。

异常后的 handler PC 不用于初始 PC 校验，也不会用异常后的 DUT 状态
重新覆盖 S0。页故障使用原有 guided execution，ECALL 使用原有指令比较路径；
其他异常的重试策略保持不变。

内存仍在首次事件时建立 REF 副本，保留普通内存、NoHype 和 COW 分支的
既有策略；这不是新增的多核内存一致性协议。

## 推进、比较与允许的同步

`difftestStep()` 先筛选架构指令边界，再调用 `diffWithNEMU()`：

1. `step_difftest_reference()` 推进 REF。普通指令执行一次，fusion 执行两次；
   SC 按既有接口同步结果，已注入中断的 REF PC 在执行前读回。
2. 严格有序访问沿用 MMIO skip 策略：先读 REF，更新下一 PC 和有效的标量
   目的寄存器，再写回。其余 REF 状态保留，写 x0 的结果不回灌。
3. 普通路径调用 `compare_difftest_state()`，检查 PC、目的寄存器和选定 CSR。
4. `difftestStep()` 根据结果执行既有 PC 重试策略，或报告并终止。

比较中仍保留部分显式同步：向量 agnostic 差异、允许跳过的计数器 CSR、
有 golden-memory 证据支持的共享内存 load/AMO 差异。
这些路径会修改 REF，不能把 `compare_difftest_state()` 当成纯函数。

## 当前检查范围与限制

- 初态中包含某个 CSR，不代表运行时会检查它。例如 `fcsr` 当前用于初始同步，
  尚未加入运行时比较。
- `mstatus` 等差异会让比较失败；`mip` 有 mask，`mip/mepc` 的现有差异路径
  仅记录和报告，不单独设置失败返回值。改变这些规则需要单独验证。
- `riscv64_CPU_regfile` 是对接 REF 的 ABI 布局，字段顺序与可选状态必须匹配
  REF 构建配置。PMP 等不在 compact snapshot 中，任意 S/VS 初态的测试还需
  配置相应的 REF 权限前提。当前 NEMU 加载仍使用旧 `difftest_init()`，尚未接入大小协商。
- 本次异常初始化入口针对 O3。SimpleCPU 的 CSR 读取已按 `tid` 选择，
  但不因此宣称其异常、MMIO 或多线程全流程与 O3 等价。
- CPU 接管保留既有 difftest 状态移交；完整 native checkpoint 恢复和多核
  首次事件时序仍需独立覆盖。

## 验证重点

优先用首条 `addi t0, t0, 1` 验证没有从提交后的状态重复执行；用非标准
reset PC 验证入口没有写死；用首个事件为页故障、中断或 MMIO 的场景验证
初始化时序。普通 CoreMark smoke 检查正常路径并比较 guest 指令数和周期数。

旧/新 REF 的初始 CSR 可以不同，初始化后必须从同一个 DUT 状态开始。
扩展 CSR 检查范围时还应主动注入 mismatch，验证错误确实导致失败；
仅有正常程序通过不足以证明检查能力。
