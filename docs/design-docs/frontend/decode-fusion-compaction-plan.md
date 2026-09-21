# Gem5 Decode 指令融合补位优化：实施与验证计划

更新日期：2026-09-20。

## 1. 基线、目的与交付边界

本次从昆明湖基准线 `xs-dev` 的固定提交
`ea8dc6b5d9c29accd6ec939cd75aaba9c5679465` 开发，记为提交 A。
工作分支为 `feature/decode-fusion-compaction`，隔离工作树为
`/nfs/home/xiongye/GEM5-decode-fusion`。原工作树
`/nfs/home/xiongye/GEM5` 及其中既有修改保持独立；不采用
`experiment/kmhv3-fetch10` 作为代码起点或性能基线。

目标是在输入仍为 8 条/拍、输出仍为 8 个 DynInst/拍的条件下，解除
Decode 一次只能消费一个 Fetch 组的限制。融合节省输出槽后，可以继续消费
FIFO 中后续输入；允许跨 Fetch 组配对，但首版不跨 FTQ。
输入持续不足时不承诺填满输出，不以模拟主机运行时间判断架构收益。

本轮交付实现、配置参数、针对实际实现的定向测试、构建和正确性验证记录，
以及模块交互审查。完整的 0.3c 性能 CI 留待后续，当前不触发 CI、提交或推送。
验证结果以实际日志为准；本计划的用例列表不代表这些测试已经通过。

核心改动位于 `src/cpu/o3/decode.cc`、`decode.hh`，另允许
`BaseO3CPU.py`、定向测试及必要的构建注册。保留原文件命名、排版和代码组织习惯，
新增代码注释使用英文，不进行无关重构或整文件格式化。

## 2. 配置与微架构合同

### 模式选择与支持边界

| 参数 | 默认值 | 含义 |
|---|---:|---|
| `enableDecodeFusionCompaction` | `False` | 启用本次融合补位路径 |
| `decodeFusionBufferSize` | `40` | 新输入 FIFO 容量，按 raw 条目计 |
| `decodeFusionScanWidth` | `16` | 每拍扫描窗口上限，含无效条目 |

仅在开关开启、配置线程数为 1、且为真实执行时使用新路径；启动时选定，
不能根据某拍只有一个活跃线程切换。False、SMT 和 Trace 模式继续走旧路径；
在不支持的模式下申请优化时报告回退。关闭补位开关不等于关闭已有指令融合。

新路径启动时拒绝 `enable_loadFusion`、`enableConstantFolding` 或
`enableMovImmElimination` 同时开启。普通 MoveElimination 和普通 load 的
值预测保持既有配置，不为本次优化额外关闭。

当前实验配置为输入/输出 8、FIFO 40、扫描 16、Fetch→Decode 延迟 3。
参数检查要求 Decode/Rename 宽度非零且一致，扫描上限非零且不超过 FIFO 容量，
FIFO 能容纳在途预留，前向延迟 `1 <= D <= backComSize`。
`enablePredecode=True` 时进一步要求 `D >= 3`，防止指令进入消费窗口时
既有 predecode/resolve 处理尚未完成。保留 `isPredecodeChecked()` 的控制检查语义。

### FIFO、扫描与配对

优化路径使用独立 FIFO 保存 DynInst 指针及来源 Fetch 组编号，后者只用于统计。
旧 fixedbuffer、stallBuffer 和组计数仍服务旧路径；新路径不依赖组边界决定消费。

每拍先检查 Commit squash，再接收原延迟通道已经到达的包；本拍无 Commit squash、
线程活跃且未被 `blockDecode` 阻塞时开始消费。入队后确定本拍最多 16 条的窗口，
跳过无效条目不能将窗口向后扩展。已到达的新条目可当拍消费，不额外添加流水级。

依次查看队头 A 和紧随其后的 B：

1. A 无效则弹出，消耗扫描额度，不产生输出；不跨无效条目找配对。
2. A 遇标量/向量边界时留在队头并停止。
3. B 在当前窗口内且 A/B 合法可融合，消费两条，产生一个输出。
4. 不可融合或 B 不可用时只消费 A，执行完整原始单项控制处理。
5. 输出达到 8、窗口耗尽、FIFO 为空或发生序列化/重定向时结束。

先决定配对再占输出槽，确保 16 条恰好 8 对可以得到 8 个融合输出。
不递归融合，不等待下一拍凑对。向量策略按本拍首条有效指令确定；
`vset` 继续设置序列化屏障并终止本拍消费。

候选须满足同线程、同有效版本、同 FTQ、同 loopIteration、序号递增和真实
fall-through PC 邻接，正确处理 2/4 字节指令长度。不以 `seqNum+1` 判断邻接。
排除 squashed、已附带 Fault、micro-op、已有融合项、控制/向量/vset、序列化及
非投机指令、错误预测为 taken 的非分支。其余类型、立即数和寄存器规则复用既有融合表。

### 融合应用、容量和复杂度

`prepareFusion` 负责准备候选，不修改输入队列、CPU 全局指令列表或成功计数；
保留 `ignoreFusionPC` 命中后可能清除超时状态、但本次仍拒绝的既有行为。
同一候选不能重复尝试绕过该拒绝。`applyFusion` 只执行一次，将原始两项替换为一个
融合 DynInst，保留两条原始 DynInst 的所有权，写入一致的线程、版本、FTQ、
loopIteration、起始 PC、结束 NPC 和 predecode 标记。依据新项实际源寄存器数设置可发射状态。

前瞻 B 不执行控制、RAS、历史、统计等消费副作用；只有真正弹出并占据输出槽的
单项指令才能调用控制处理。恢复路径不得清除本拍已确定的更老输出或触发指令。

令 `Q` 为接收和消费结束后的库存、`W` 为每拍输入宽度、`D` 为前向延迟、
`B` 为 FIFO 容量。普通发送许可为：

```text
Q + D * W <= B
当前配置：Q + 3 * 8 <= 40，即 Q <= 16
```

CPU 每拍先 Decode 后 Fetch，因此预留覆盖 `D-1` 包在途数据及本拍可能新发的一包，
不依赖下一拍继续消费。入队前检查实际空间，禁止循环缓冲区覆盖队头。
本拍 selfSquash 强制阻塞优先于空间许可。所有出口统一更新持久 `blockFetch`、
阻塞原因、输出停顿信息和阶段活动状态。

正常消费复杂度为 `O(S)`，`S` 是有限扫描窗口；额外库存为 `O(B)`。
后端 squash 采用单次队头弹出/幸存项尾部追加的保序筛选，检查所有 FIFO 条目并保留有效的较老项，复杂度 `O(B)`，
不能因逐项移动整个循环缓冲区引入无界工作。恢复窗口和队列容量都不因融合率变化而增长。

## 3. 模块交互与统计

更完整的接口分析见 [模块交互评审](decode-fusion-interaction-review.md)。

- Fetch：接收与消费分离；阻塞时仍接收在途输入。保留延迟 redirect 到达 Fetch 后的
  再次清理，保留 Commit 重定向优先级。原前向延迟和通信格式不变。
- Rename：`toRename->size` 为实际对象数，最多 8；不将扫描数 16 写入输出数量。
  新基线 Rename 已按条目线程号分流混合包；本轮仍只在单线程启用新路径。
- 恢复：版本比较使用支持回绕的 `SquashVersion`，同时检查 `isSquashed()`。
  后端 squash 按 `doneSeqNum` 删除年轻项及已 squash 项；较老幸存项保序并更新版本，
  当拍到达的幸存项也按相同边界处理，避免 Rename 用新版本误拒绝它们。
  发生后端 squash 的拍不发送输出；不得无条件清空所有较老库存。
- 控制：保留非分支错误 taken、直接分支、普通 return 的 RAS、非投机返回及分支历史处理。
  已附带 Fault 的有效指令单项输出，不能当作无效输入丢弃。
- Commit/执行：复用融合执行和精确异常重放协议、`IsFusion` 及原始指令绑定，
  保留 difftest 对融合项的两步参考执行。
- 生命周期：新 FIFO 接入 `clearStates()`、reset、takeover、squash、selfSquash、
  drain 和活动维护，避免漏清库存、提前 drain 或永久停滞。
- 工具：保留 O3PipeView 与 PerfCCT 的原始指令记录形式，不复制融合项 fetchTick
  生成重复 seqNum 记录，不重建 PerfCCT 元数据覆盖 Fetch 历史。
  Decode 调试消息记录融合关系，本轮不设计新可视化格式。

每拍定义 `C` 为有效 raw 消费数、`X` 为正常扫描丢弃数、`F` 为融合对数、
`U` 为实际输出数，应满足：

```text
U = C - F
C + X <= decodeFusionScanWidth
U <= decodeWidth
```

整批 squash 清理单独统计。新增指标直接对应实现：

| 指标 | 口径 |
|---|---|
| `compactionInputInsts` | 接收到 FIFO 的 raw 条目数，含无效条目 |
| `compactionRawInsts` | 正常消费的有效 raw 指令数 C |
| `compactionDiscardedInsts` | 在扫描窗口中丢弃的无效条目数 X |
| `compactionFlushedInsts` | squash 从队列中删除的条目数 |
| `compactionFusedPairs` / `compactionOutputInsts` | F / U |
| `compactionCrossBundleFusions` | 配对两项来自不同 Fetch 组的次数 |
| `compactionFetchBlockedCycles` | 新路径阻塞 Fetch 发送的拍数 |
| `compactionRawPerCycle` / `compactionOutputPerCycle` | 每拍 C / U 分布 |
| `compactionOccupancy` | 消费结束后 FIFO 库存分布 |
| `compactionStopReasons` | 每拍一个停止原因 |

停止原因包含 `inputEmpty`、`outputFull`、`scanLimit`、`vectorBoundary`、
`serialize`、`redirect`、`backendBlocked`、`squash`。输出停顿向量仍为 8 槽；
旧路径统计行为保持原样，新路径 Decode 空槽按实际输出 U 计算。

## 4. 验证步骤与判据

以下保留验证要求和判据；实际已执行结果、证据和未覆盖边界见
[本地验证记录](decode-fusion-validation.md)。不能把用例清单等同于全部已通过。

1. 固定提交 A、构建设置和配置；在新实现关闭时复跑代表性基线，核对架构结果、
   退出状态、模拟周期及关键统计，不只对比四舍五入后的分数。
2. 构建优化二进制及针对实际 Decode 实现的定向测试；测试不是另写一套概念算法。
3. 执行短程真实指令工作负载和 difftest，核对控制流、寄存器、访存及精确恢复。
4. 验证 SMT/Trace 回退和不支持组合的启动诊断；检查 Predecode 延迟边界。
5. 开启 O3PipeView、PerfCCT 并运行现有解析/查询入口；确认格式兼容并记录既有显示限制。

定向场景包括空队列、短包、无融合和多融合，16 条恰好 8 对，8/9 与 16/17 边界；
无效条目阻断配对；跨 Fetch 组同 FTQ 成功、跨 FTQ 拒绝；剩一槽时 B 触发 redirect；
标量/向量、vset、压缩指令、Fault、禁融重放；持续后端阻塞、容量上界、在途数据；
Commit/selfSquash、版本回绕、较老幸存项和迟到旧路径；非空队列线程退出、drain、takeover。

验收要求：无指令丢失或重复、无缓冲覆盖、无错误路径逃逸或恢复 PC 错误、
无永久反压或 drain 停滞。工具现有入口无新增回归。未执行、不可用或仅静态覆盖的项
逐项标明，不能用一次构建或短程 difftest 代替全部覆盖。

## 5. 后续三组性能 CI 对照

使用 `xs-dev` 配套的 Manual Performance Test；固定工作流、编译环境、REF、
checkpoint 清单和权重。优化提交 B 必须由提交 A 加本次改动形成，False/True 使用同一 B。

| 任务名称 | 被测源码 | Extra gem5 args |
|---|---|---|
| `baseline-for-decodeinst` | 原始提交 A | 留空，不传尚不存在的新参数 |
| `fause-for-decodeinst` | 优化提交 B | `--param=system.cpu[0].enableDecodeFusionCompaction=False` |
| `true-for-decodeinst` | 同一提交 B | `--param=system.cpu[0].enableDecodeFusionCompaction=True` |

`fause` 保留用户指定的任务名称，布尔值必须拼作 `False`。三组均用 `kmhv3.py`、
`spec06-rva23-novec-gcc16-0.3c`，benchmark 过滤及自定义 checkpoint/权重留空；
不添加 `--ideal-kmhv3` 或额外微架构覆盖。实际 CI 输入与产物需在触发前再次核对。
本轮不触发这三组 CI。

完整性能验收要求三组使用相同的 145 个有效 checkpoint，覆盖 29 个子项，
并保存实际配置、模拟统计、分数文件、权重和参考模型标识。缺失项不得静默剔除后重新归一化。

- A 对 B-False：检查引入新代码后旧路径是否保持行为。
- B-False 对 B-True：测量补位路径及其配对约束的净性能变化。
- A 对 B-True：观察相对原始基线的最终变化。

报告总分、INT、FP 和全部子项；收益为 `(True / False - 1) * 100%`。
不把 CI 页面的整数分组摘要当作全部 29 项总分，不把主机耗时当作架构性能。
如 A/False 周期或关键统计不一致，先查配置和旧路径回归，再解释 True 的性能差异。
