# KMHV3 非访存向量功能单元与 Issue Queue 数量对齐

## 目标与范围

本阶段按 RTL 默认 `vecSchdParams` 对齐 **4 条向量计算 IQ 的数量、容量、入队口数、发射口数和 FU 组合数量**，不修改向量访存地址/数据队列、`vset*` 路径和执行延迟。RTL 基线为 XiangShan `c8d7b3a5c1ab`、`EnableBackendV2Config=false`；原 GEM5 配置是 1 条 42 项的 `vecIQ0`，含 5 个能力相同的 `SIMD_Unit` 发射口。参见 [RTL 参数](https://github.com/OpenXiangShan/XiangShan/blob/c8d7b3a5c1abf3f42c954e61abba20dd27e02a21/src/main/scala/xiangshan/Parameters.scala)、[`KMHV3Scheduler`](../../../configs/common/FUScheduler.py) 和[总体对齐方案](vector-issue-queue-alignment-plan.md)。

| GEM5 IQ | 对应 RTL EXU | 表项 | 入队口 / 发射口 | 同一发射口上配置的 FUDesc |
| --- | --- | ---: | ---: | --- |
| `vecIQ0` | VFEX0 | 16 | 2 / 1 | `VecIALU`、`VecIMAC`、`VecMOVE`、`VecFCVT`、`VecFMAC` |
| `vecIQ1` | VFEX1 | 16 | 2 / 1 | `VecIALU`、`VecIDIV`、`VecFMAC`、`VecFDIV` |
| `vecIQ2` | VFEX2 | 16 | 2 / 1 | `VecIALU` |
| `vecIQ3` | VFEX3 | 16 | 2 / 1 | `VecIALU` |

因此是 **4 条独立 IQ、64 个合计表项、4 个计算发射口、11 个 FUDesc 配置位置**；按类型分别为 `VecIALU ×4`、`VecFMAC ×2`，`VecIMAC/VecMOVE/VecFCVT/VecIDIV/VecFDIV` 各 1。FUDesc 是 GEM5 发射能力的描述，**不是另外 11 个独立发射端口**。每队列入队带宽为 2；`scheduleToExecDelay=3` 沿用原计算 IQ 的值，不是额外添加的等待周期。

## 建模合同与实现

资源链为“向量非访存指令入队 → 目标 IQ 表项/入队口竞争 → 源就绪及每 IQ 单发射口仲裁 → 原有执行流水”。四条 IQ 分别提供满队列背压，不能通过其他队列的空位无条件借用容量；保留原有 flush、依赖追踪和唤醒网络对向量计算指令的覆盖。单个 IQ 最多 16 个驻留项；继续复用有界的 ready 队列与原调度器，不引入每周期全局扫描。

- [`FuncUnitConfig.py`](../../../src/cpu/o3/FuncUnitConfig.py) 按 RTL FU 名称增加七种 FUDesc；原 `SIMD_Unit` 保留给其他调度器。
- [`FUScheduler.py`](../../../configs/common/FUScheduler.py) 只修改 `KMHV3Scheduler` 的计算 IQ；四条新 IQ 都参与原 FP/向量和内存→向量 wakeup 网络。`kmhv3.py` 和 `idealkmhv3.py` 均选择 `KMHV3Scheduler`，因此两者都会使用新拓扑；未改变独立的 `IdealScheduler`。
- [`issue_queue.cc`](../../../src/cpu/o3/issue_queue.cc) 构建 `dispTable` 时，每个 IQ 对同一 OpClass 只登记一次。一个端口上的多个 FUDesc 可能声明同一 OpClass；若不去重，同一 IQ 会多次进入候选列表，被误当作多个队列，导致分派比例及队列选择失真。去重只发生在初始化阶段。
- `ld0～2`、`sta0～1`、`std0～1` 保持原状；没有增加 VSTD IQ 或改变向量访存拆分。`VectorConfigOp` 仍走整数队列 `intIQ5`，不是这里的四条向量计算 IQ。

## 本阶段的精度边界

GEM5 目前按 **OpClass** 分派和选择端口，尚未为每条向量指令标记 RTL FU 类型。例如整数 ALU、MAC、除法同为 `VectorIntegerArithOp`，浮点 MAC 和除法同为 `VectorFloatArithOp`。因此本阶段**仅对齐声明的 FU 数量、IQ 数量和队列资源**，尚不能保证 `vdiv` 只能选择 VFEX1，或整数 MAC 只能选择 VFEX0。不能把 11 个 FUDesc 理解成已经实现 11 种相互隔离的物理资源；要对齐指令级可达性，后续必须增加译码时的 FU 分类并让 Dispatch/issue 使用同一分类。

目前的 OpClass 近似映射见下表；同一 IQ 上的同类 OpClass 重叠由 `dispTable` 去重处理，延迟和 pipelined 配置沿用原 `SIMD_Unit`：

| FUDesc | 当前 OpClass 覆盖 | 不能精确区分的情形 |
| --- | --- | --- |
| `VecIALU` | `VectorIntegerArith`、`VectorIntegerReduce`、`VectorIntegerExtension` | 其中也包括被归到相同 OpClass 的整数除法/MAC |
| `VecIMAC`、`VecIDIV` | `VectorIntegerArith` | 二者及 `VecIALU` 当前共享 OpClass |
| `VecMOVE` | `VectorMisc` | 此类还包含非移动指令 |
| `VecFCVT` | `VectorFloatConvert` | 转换子类型尚未细分 |
| `VecFMAC`、`VecFDIV` | `VectorFloatArith`；`VecFMAC` 还覆盖 `VectorFloatReduce` | 浮点普通运算、MAC、除法尚未分离 |

**风险**：虽然各 IQ 容量和总发射口数已改变，按粗粒度 OpClass 计算的每类指令可用队列仍可能比 RTL 更多或更少；本阶段不能依据总 IPC 推断 FU 类型路由已对齐。`deferNewEnqueueSelection`、入口/fast 项转换、RF 仲裁、发射取消以及向量访存路径均未重新建模。

## 验证与后续

配置测试 [`pyunit_kmhv3_vector_iqs.py`](../../../tests/pyunit/pyunit_kmhv3_vector_iqs.py) 校验四条 IQ 的 FUDesc 组合、容量/口数、旧计算 OpClass 的覆盖/候选队列，以及访存/FP→向量的 wakeup 连通性。

- `scons build/RISCV/gem5.opt --gold-linker -j8`：构建通过。
- `build/RISCV/gem5.opt --quiet tests/pyunit/pyunit_kmhv3_vector_iqs.py`：3 个测试通过。
- `tests/run_pyunit.py`：新增的 3 个测试在整套测试发现流程中通过；整套 27 个测试中另有 6 个现有资源测试因 Python 3.12 不再提供 `unittest.TestCase.assertEquals` 而报错，与本次 IQ 修改无关。
- 使用 `configs/example/se.py --ideal-kmhv3` 运行仓库自带 RISC-V `hello`：正常退出；生成的 `config.ini` 含四条新 IQ，证明 C++ 调度器可以创建，但该程序没有运行向量指令。
- 另尝试运行汇编的 RVV 整数/浮点短程序：SE 模式在第一条 `vsetvli` 报告 `Vector state is off`，**尚未执行到向量计算 IQ**。这是本轮验证缺口；本次未为测试改变 ISA/SE 状态初始化。

后续需要具备向量状态的工作负载，在相同环境下比较各 IQ 的 `insertDist`、`issueDist`、`portissued`、`avgInsts` 和 `portBusy` 与 RTL 的分队列压力。无相同 workload 与 RTL trace 时，仅能宣称**资源拓扑与声明数量对齐**，不能宣称性能趋势或逐拍行为已验证。
