# O3 Reverse Tick 与新版阻塞机制说明

## 1. 文档目标

本文档用于解释最近这组 O3 流水线重构的设计意图与核心机制，重点回答下面几个问题：

1. 这个 PR 到底想解决什么问题。
2. 新版流水线里 `TimeBuffer`、本地 buffer、`StallSignals` 分别负责什么。
3. 这套机制和 RTL 中常见的 `valid-ready`、级间寄存器、FIFO 分别是什么对应关系。
4. 后续开发者在修改代码时，应该把什么信息放进 `TimeBuffer`，什么逻辑放进 `StallSignals`。
5. 这套重构为什么更适合后续做 SMT 的分级阻塞。

本文档描述的主改动主要来自以下提交：

- `d966055b9d`: `cpu-o3: using reverse ordered tick & refactor the stalls logic`
- `fdba12d73e`: `cpu-o3: fix reverse ordered tick`

`e6d58acd5e` 主要是 perfCCT 记录增强，不是本文的重点。

---

## 2. 一句话总结这个 PR

这个 PR 的核心不是修改某个预测器或某个调度算法，而是**重构 O3 流水线的时序建模方式**。

可以用一句话概括：

- 指令仍然沿着 `Fetch -> Decode -> Rename -> IEW -> Commit` 向前流动。
- 阻塞/背压则改成沿着 `Commit -> IEW -> Rename -> Decode -> Fetch` 同拍反向传播。

也就是说，这个 PR 想做的是：

- 让后级先决定“我这拍能不能接收”
- 再让前级在同一拍立即感知这个结果
- 从而减少旧模型中大量 `block / unblock / skidBuffer / state machine` 的补偿逻辑

这套结构的一个重要目标，是为后续 SMT 下的按线程阻塞和仲裁打基础。

---

## 3. O3 流水线的基本背景

这个 O3 CPU 的主链路可以先粗略理解为：

```text
Fetch -> Decode -> Rename -> IEW -> Commit
```

各级职责大致如下：

- `Fetch`
  - 取指
  - 与分支预测器、FTQ、Icache 交互
- `Decode`
  - 解码
  - 做部分早期控制流检查
  - 做部分指令融合
- `Rename`
  - 进行物理寄存器分配与重命名
  - 处理 rename map / history buffer 等资源
- `IEW`
  - 分发到 IQ / LSQ
  - 发射、执行、写回
- `Commit`
  - 通过 ROB 按程序序退休
  - 处理 trap、squash、精确异常等

CPU 的顶层 tick 入口在 `src/cpu/o3/cpu.cc`。

---

## 4. 旧模型与新模型的最重要区别

### 4.1 旧模型的 tick 顺序

在这个 PR 之前，CPU 的 tick 顺序基本是：

```text
fetch -> decode -> rename -> iew -> commit
```

这意味着：

- 前端先运行
- 后端后运行
- 如果后端在这一拍才发现资源满、需要 squash 或者需要阻塞，那么前端往往已经把这一拍干完了

因此旧模型需要维护很多额外状态来“补时序差”，例如：

- `Blocked`
- `Unblocking`
- `Squashing`
- `SerializeStall`
- `skidBuffer`
- block/unblock 握手信号

这些东西并不是无意义的，它们是在旧 tick 顺序下为了保持行为正确而产生的。

### 4.2 新模型的 tick 顺序

这个 PR 之后，tick 顺序改成：

```text
commit -> iew -> rename -> decode -> fetch
```

注意，这并不表示“指令反向流动”。

真正变化的是：

- 软件模拟时，后级先更新自己的状态
- 然后前级在同一拍根据后级状态决定是否推进

所以新模型下更自然的背压链是：

```text
Commit -> IEW -> Rename -> Decode -> Fetch
```

也就是：

1. `Commit` 先判断自己是否还能接收新的 rename 结果、ROB 是否还能插入
2. `IEW` 根据 commit 的反馈决定是否继续向前接收
3. `Rename` 根据 IEW/资源情况决定是否继续 rename
4. `Decode` 根据 rename 是否能继续推进来决定是否阻塞 fetch
5. `Fetch` 最后在这一拍就能知道自己能不能继续往前送

这就是所谓的 reverse ordered tick。

---

## 5. `TimeBuffer` 到底是什么

### 5.1 不要把它简单理解成“普通变量”

`TimeBuffer` 不是普通共享变量，它更像“带显式时间偏移的环形缓冲”。

关键语义如下：

- `getWire(0)`：访问当前时刻的槽位
- `getWire(-delay)`：读取延迟 `delay` 拍后才可见的槽位
- `advance()`：在周期边界推进整个时间窗口

因此，它的主要用途不是“简单共享一块数据”，而是**显式建模 stage 间传播延迟**。

实现见：

- `src/cpu/timebuf.hh`

### 5.2 `TimeBuffer` 不是 RTL 意义上的“自动保值级间寄存器”

这点非常重要。

在 RTL 里，我们通常会这样想：

- 上一级把 `valid + data` 放在级间寄存器或 FIFO 中
- 如果下一级 `ready=0` 没收走，那么这个寄存器/FIFO 里的值会继续保持

而在这个 gem5 O3 重构里，语义被拆开了：

1. `TimeBuffer`
   - 主要负责“这个信息跨多少拍可见”
   - 更像“带延迟的链路”或“时间总线”
2. 本地 buffer / 队列 / ROB / LSQ
   - 主要负责“这个数据如果本拍没被处理，要继续留着”
3. `StallSignals`
   - 主要负责“这一拍是否允许上游继续推进”

所以一句话记忆就是：

**`TimeBuffer` 更像延迟总线，而不是永远替你保值的流水寄存器。**

### 5.3 那 `TimeBuffer` 会不会覆盖旧值

会。

`TimeBuffer` 随着每拍 `advance()` 会推进时间窗口，并重建未来槽位。也就是说：

- 它的内容不是“只要没人消费就一直保留”
- 它本质上是按时间流动的

因此，**真正需要长期保留的数据，不能只依赖 `TimeBuffer` 保存**。

如果某一级已经收到数据但暂时处理不了，正确做法应该是：

- 先把数据搬入该 stage 的本地 buffer
- 然后由本地 buffer 持有这些数据直到后续周期继续处理

这也是这次 PR 中大量引入 `fixedbuffer` 的直接原因。

---

## 6. 新模型下有哪些“存储/传输”层次

为了理解新版阻塞机制，最好的方法是把整个流水线拆成三层看。

### 6.1 第一层：链路延迟层

负责表达“这个信息多少拍后才能被对面看到”。

对应机制：

- `TimeBuffer<TimeStruct>`
- `fetchTimebuffer`
- `decodeTimebuffer`
- `renameTimebuffer`
- `iewTimebuffer`

它们表达的是：

- 前向指令流的级间延迟
- squash / redirect / doneSeqNum 等反馈控制信息的延迟

### 6.2 第二层：本地保存层

负责表达“这条数据已经到我这里了，但我这拍不一定能处理完”。

对应结构：

- `Decode::fixedbuffer`
- `Rename::fixedbuffer`
- `IEW::fixedbuffer`
- `Commit::fixedbuffer`
- 以及更传统的 `fetchQueue`、`ROB`、`LSQ`、`IQ`

这一层最接近 RTL 里“寄存器/FIFO 保值”的语义。

### 6.3 第三层：同拍背压层

负责表达“我这一拍到底准不准继续推进”。

对应结构：

- `StallSignals`
  - `blockFetch`
  - `blockDecode`
  - `blockRename`
  - `blockIEW`

这一层最接近 RTL 里 `ready/allowin` 那一半的语义。

---

## 7. `StallSignals` 是什么

### 7.1 它的本质

`StallSignals` 是这次 PR 新增的“快速背压通道”。

它不是用来装复杂 payload 的，也不是用来表达多拍传播的，而是用来表达：

- 后级这拍能不能接收
- 上游这拍是不是该停

它是这条同拍背压链的载体：

```text
Commit -> IEW -> Rename -> Decode -> Fetch
```

### 7.2 为什么它存在

如果仍然让“阻塞信息”完全通过 `TimeBuffer` 传播，那么由于 `TimeBuffer` 是按拍延迟可见的，前级会晚一拍知道后级已经堵住。

一旦晚一拍，就又需要：

- block / unblock 状态机
- skid buffer
- 额外的中间状态

也就是说，`StallSignals` 的存在，就是为了把“是否准许推进”这类必须同拍生效的控制，从 `TimeBuffer` 中分离出来。

### 7.3 它是不是握手信号

可以说它**类似 RTL `valid-ready` 协议中 `ready/allowin` 的那一半**，但并不是完整的 `valid-ready` 通道抽象。

原因如下：

1. 这里没有一个统一的 `valid` 信号字段
   - 是否有数据，通常由 `size`、队列是否为空、本地 buffer 是否为空等方式表达
2. `ready` 也没有被统一抽象成一个接口
   - 而是拆成了 `blockFetch / blockDecode / blockRename / blockIEW`
3. 数据保持不靠通道本身完成
   - 而是靠各级自己的本地 buffer / ROB / LSQ 来完成

所以更准确地说：

- `StallSignals` 是“同拍背压”的建模机制
- 它与 RTL 中的 `ready/allowin` 很相似
- 但它不是完整 `valid-ready` 协议的一比一翻版

---

## 8. 与 RTL `valid-ready` / FIFO / 级间寄存器的对应关系

这一节最容易帮助硬件背景的开发者建立直觉。

### 8.1 RTL 中的典型模型

在 RTL 中，一个级间通道通常同时具备三层含义：

1. **数据通道**
   - `data`
2. **有效性**
   - `valid`
3. **可接收性**
   - `ready`

握手条件一般是：

```text
valid && ready
```

一旦握手成功：

- 数据向下一级推进

如果握手不成功：

- 数据保持在当前级间寄存器/FIFO 中
- 等待后续周期重试

### 8.2 新版 gem5 O3 中的对应关系

可以用下面这张表理解：

| RTL 概念 | 新版 gem5 O3 对应物 | 说明 |
| --- | --- | --- |
| 级间链路延迟 | `TimeBuffer` | 表达“几拍后可见” |
| 级间寄存器/FIFO 中的数据保持 | 各 stage 的 `fixedbuffer`、`fetchQueue`、`ROB`、`LSQ` 等 | 表达“已收到但暂未消费的数据继续保留” |
| `ready/allowin` 背压 | `StallSignals` | 表达“这一拍能不能继续往前推” |
| `valid` | `size`、队列非空、buffer 非空等隐式条件 | 没有统一字段，分散在具体结构里 |
| payload 控制信息 | `TimeStruct` / 前向 Struct | squash、redirect、doneSeqNum 等 |

### 8.3 一个更直观的类比

如果用硬件设计语言来类比，新模型更像下面这种结构：

```text
前向链路: 有延迟的数据通道
本地级内: 有输入缓冲 / FIFO / 寄存器保存未消费数据
反向链路: 有组合式 allowin / backpressure
```

而不是一个统一的“每一级都只靠一组 valid-ready 寄存器”的极简模型。

### 8.4 这和“寄存器保值”的关系

RTL 中你会说：

- 级间寄存器保值

新版 gem5 O3 中更合适的说法是：

- **接收级自己的本地 buffer 保值**

也就是说，不是 `TimeBuffer` 帮你 hold 住数据，而是：

- 下一级先把输入搬进自己的本地 buffer
- 如果这拍处理不完，本地 buffer 继续保留
- 上游则通过 `StallSignals` 被挡住，不再继续注入更多数据

这是和 RTL 直觉最像、但实现手法不同的地方。

---

## 9. 这次 PR 中各机制分别承担什么责任

### 9.1 `TimeBuffer` / `TimeStruct`

适合承载：

- 带 payload 的控制信息
- 明确需要跨拍延迟传播的信息
- stage 间前向传递的指令包

典型例子：

- squash
- redirect PC
- `doneSeqNum`
- `doneMemSeqNum`
- resolved CFI
- 各级向下一阶段发送的指令 bundle

### 9.2 本地 `fixedbuffer`

适合承载：

- 已经到达本 stage，但本拍还没处理完的数据
- 需要跨拍继续保留的输入

典型场景：

- Decode 已经收到了来自 Fetch 的若干指令，但这拍只能处理一部分
- Rename 已经收到了来自 Decode 的指令，但这拍资源不够
- IEW 已经收到了来自 Rename 的指令，但 IQ / LSQ / dispatch 带宽不足
- Commit 已经收到了来自 Rename 的指令，但这拍不一定能全部插入 ROB

### 9.3 `StallSignals`

适合承载：

- 这一拍是否允许上游继续推进
- 没有复杂 payload 的简单 backpressure

典型语义：

- `blockIEW`：commit 告诉 IEW，这一拍不要再继续向前推进到自己这里
- `blockRename`：IEW 告诉 rename，这一拍别再往下送
- `blockDecode`：rename 告诉 decode，这一拍别再往下送
- `blockFetch`：decode 告诉 fetch，这一拍别再继续出队/送指令

---

## 10. 为什么 `TimeBuffer` 和 `StallSignals` 不能简单视为同一种东西

这是后续维护最容易混淆的点。

### 10.1 它们解决的是两类不同问题

`TimeBuffer` 解决的是：

- 延迟传播
- 历史/未来时间槽位
- 带 payload 的信息移动

`StallSignals` 解决的是：

- 同拍背压
- 不需要等待 `advance()`
- 通常不带复杂 payload

### 10.2 为什么不能简单“全都塞进 `TimeBuffer`”

如果把同拍背压也塞回 `TimeBuffer`，会重新出现旧问题：

- 前级晚一拍知道后级阻塞
- 需要恢复 block/unblock 状态机来补时序差

### 10.3 为什么也不能简单“全都改成 `StallSignals`”

如果把 squash、redirect、doneSeqNum、payload 丰富的控制也都改成 `StallSignals`，会出现另一个问题：

- 这些信息本来就具有明确的多拍传播语义
- 且往往需要携带复杂数据
- 仅靠一个简单的 block 信号无法表达

因此两者在语义上是分工明确的。

---

## 11. 那有没有机会把两者“合并”

### 11.1 结论

**可以考虑做接口层统一抽象，但不建议简单把当前实现硬合并。**

### 11.2 为什么“真正合并”很难

因为语义上它们仍然是两类东西：

- 一类是 delayed payload channel
- 一类是 same-cycle backpressure

即使未来要统一成一个更抽象的 `Channel` 类，内部通常也仍然需要区分：

- 是否带延迟
- 是否需要 payload
- 是否负责 buffering
- 是否负责 backpressure

换句话说：

- **实现接口可以统一**
- **语义层面依然需要分层**

### 11.3 更合理的长期方向

如果后续 SMT 和通道复杂度继续上升，可以考虑做更高层的抽象，例如：

- `DelayedPayloadChannel`
- `BackpressureChannel`
- `BufferedStageInput`

但在当前阶段，更重要的是：

1. 先把语义边界写清楚
2. 先避免后续开发者误用
3. 再考虑是否值得抽象统一

---

## 12. 这一版设计为什么更适合 SMT

这个 PR 不是完整 SMT 支持，但它在结构上更适合未来做 SMT。

原因包括：

1. `StallSignals` 本身就是按线程数组组织的
2. 多个 stage 先把输入搬入本地 buffer，再决定消费多少，这更接近 per-thread arbitration 的思路
3. 背压链显式化后，更容易按线程分析“是哪个下游线程/资源导致了阻塞”

因此更准确地说：

- 这不是“SMT 已经实现完成”
- 而是“流水线控制结构更适合后续 SMT 开发”

---

## 13. 一次典型周期里会发生什么

下面用一个典型例子说明新模型的直觉。

假设这一拍后端资源紧张，Commit 或 IEW 已经接近满载。

### 周期内顺序

1. `Commit` 先 tick
   - 判断 ROB 是否还能接收、是否正在 squashing
   - 设置 `blockIEW`
2. `IEW` 再 tick
   - 读取 `blockIEW`
   - 决定自己是否还能继续从 Rename 接收
   - 必要时设置 `blockRename`
3. `Rename` 再 tick
   - 读取 `blockRename`
   - 决定这拍是否继续 rename
   - 必要时设置 `blockDecode`
4. `Decode` 再 tick
   - 读取 `blockDecode`
   - 决定是否阻塞 Fetch
   - 设置 `blockFetch`
5. `Fetch` 最后 tick
   - 读取 `blockFetch`
   - 决定是否继续向 Decode 发送指令
6. 周期末统一 `advance()` 各类 `TimeBuffer`

### 需要注意的点

在这个过程中：

- 指令本身的前向传播，仍然依赖前向 `TimeBuffer`
- 某一级已经收到但本拍处理不完的数据，会保存在该级本地 buffer 中
- 是否允许上游继续推进，则由 `StallSignals` 同拍决定

---

## 14. 开发者修改代码时的判断准则

后续如果要继续修改这套流水线代码，建议始终按下面的准则判断。

### 14.1 什么时候应该用 `StallSignals`

当你想表达的是：

- “这一拍我能不能接收”
- “上游这一拍应不应该继续推”
- “这个决策需要同拍生效”

此时优先考虑 `StallSignals`。

### 14.2 什么时候应该用 `TimeBuffer`

当你想表达的是：

- “这个信息需要延迟若干拍后可见”
- “这个信息不是简单 bool，而是带 payload”
- “这是一个控制事件，而不是简单 backpressure”

此时优先考虑 `TimeBuffer` / `TimeStruct`。

### 14.3 什么时候应该放进本地 buffer

当你想表达的是：

- “数据已经到达本级”
- “但这拍没处理完”
- “需要跨拍继续保留”

此时应该放进本级的 `fixedbuffer` 或其他局部队列/结构，而不是指望 `TimeBuffer` 自动保值。

---

## 15. 后续维护最容易踩的坑

### 15.1 把“应当同拍生效”的信号误放进 `TimeBuffer`

后果：

- 信号晚一拍
- 前级多推进一拍
- 又要重新引入复杂 block/unblock 补偿逻辑

### 15.2 把“应当带 payload/带延迟”的控制误简化成 `StallSignals`

后果：

- squash / redirect / doneSeq 等复杂语义表达不完整
- 时序语义变得含糊

### 15.3 误以为 `TimeBuffer` 会自动替 stage 持有未消费数据

后果：

- 数据保留逻辑写错
- 修改后很容易出现“本拍看起来没问题、下拍数据没了”的问题

### 15.4 把本地 buffer 和上游背压脱钩

如果一个 stage 已经出现局部积压，但没有及时通过 `StallSignals` 阻止上游继续输入，就容易出现：

- 带宽模型失真
- buffer overflow
- 行为与真实流水线不一致

---

## 16. 推荐的阅读路径

如果后续开发者要读这套逻辑，建议按下面顺序看：

1. `src/cpu/o3/cpu.cc`
   - 先看 `CPU::tick()` 的新顺序
2. `src/cpu/timebuf.hh`
   - 理解 `TimeBuffer` 的真正语义
3. `src/cpu/o3/comm.hh`
   - 理解 `TimeStruct` 和 `StallSignals` 的职责边界
4. `src/cpu/o3/commit.cc`
   - 看 `blockIEW` 和 squash 源头
5. `src/cpu/o3/iew.cc`
   - 看 `blockRename`、dispatch、本地 buffer
6. `src/cpu/o3/rename.cc`
   - 看 `blockDecode`、本地 buffer、rename 资源约束
7. `src/cpu/o3/decode.cc`
   - 看 `blockFetch`
8. `src/cpu/o3/fetch.cc`
   - 看 fetch 最终如何消费背压和控制反馈

---

## 17. 最后一句总结

这次重构之后，理解新版流水线最有效的方式不是问：

- “为什么 `TimeBuffer` 不像 RTL 寄存器那样自动 hold 数据？”

而是要改成下面这个三层心智模型：

1. **`TimeBuffer` 负责延迟传播**
2. **本地 buffer / ROB / LSQ 负责保存未消费数据**
3. **`StallSignals` 负责同拍背压**

如果始终用这三层模型来读代码，后续对新版阻塞逻辑、reverse tick 和 SMT 扩展方向都会更容易理解。
