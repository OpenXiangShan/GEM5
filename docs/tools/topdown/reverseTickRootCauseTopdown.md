# Reverse Tick 下的 Root-Cause Topdown 口径说明

## 1. 目标

本文档用于说明新版 reverse ordered tick 流水线下，`fetchStallReason`、`decodeStallReason`、`renameStallReason`、`dispatchStallReason` 这几组 topdown 统计应该如何理解，以及为什么需要把根因按口径 B 从后向前传播。

本文档假设：

- `dispatchStallReason` 是最接近真实后端瓶颈的位置
- 前端与中端的 stall reason 不是为了替代 `dispatchStallReason`
- 而是为了描述同一个根因向前传播后，在不同流水级造成了多少停顿与空槽

---

## 2. 推荐口径：口径 B（根因归因）

这里采用的口径是：

- 如果真正的瓶颈发生在后端，则应该把这个根因沿着背压链一路向前传播
- 前面的 stage 在被动停住时，不应该把问题重新归因为“本级无事可做”

也就是说：

- `dispatchStallReason` 负责回答“真正的病灶在哪里”
- `fetch/decode/rename StallReason` 负责回答“这个病灶向前传播后，分别压住了哪些 stage”

---

## 3. 四级统计各自的角色

### 3.1 `dispatchStallReason`

这是最重要的一组统计。

它最接近实际资源瓶颈发生的位置，适合用于定位真正的优化方向，例如：

- IQ 满
- LQ / SQ 满
- serialize
- ROB head not ready
- 长执行延迟
- memory bound

因此：

- `dispatchStallReason` 应作为最终根因锚点
- 后续性能分析优先看这一组

### 3.2 `renameStallReason`

这一组统计表示：

- 后端根因已经传播到 rename
- rename 这一拍因为下游原因或资源约束而无法继续推进

它的作用是帮助判断：

- 后端瓶颈是否已经明显压到了 rename
- rename 的停顿有多少是“本级资源不足”，有多少是“被后端反压”

### 3.3 `decodeStallReason`

这一组统计表示：

- 同一个根因继续往前传播后，对 decode 造成了多少影响

它的价值主要在于：

- 判断 decode 空槽到底是 frontend 自己的问题，还是下游反压造成的

### 3.4 `fetchStallReason`

这一组统计表示：

- 最终反映到前端 delivery 上的停顿原因

它非常适合区分两类情况：

1. 真正的 frontend 问题
   - `IcacheStall`
   - `ITlbStall`
   - `FTQBubble`
   - 其他本级取指问题
2. 后端根因一路反压到前端
   - 例如某类 memory bound、serialize、commit squash、长执行等

因此：

- `fetchStallReason` 不应直接替代 `dispatchStallReason`
- 它的主要价值是区分“前端真的有问题”还是“前端只是被后端压住了”

---

## 4. 为什么这不是重复统计

表面上看，四级都可能记录同一个 `StallReason`，但它们不是在统计同一个事件，而是在统计：

- 同一个根因在不同 stage 上造成的影响范围

例如，真实根因是 `LoadL2Bound`：

1. `dispatchStallReason=LoadL2Bound`
2. rename 因为 dispatch 不再前推，也记录 `LoadL2Bound`
3. decode 因为 rename 被压住，也记录 `LoadL2Bound`
4. fetch 因为 decode 不再接收，也记录 `LoadL2Bound`

这四条统计放在一起的含义是：

- 问题源头在后端 load miss
- 并且这个问题已经足够严重，向前压住了整条流水线

---

## 5. Root-cause 传播规则

为了让统计口径稳定，推荐采用如下规则。

### 规则 1：本级真实原因优先

如果本级确实发生了明确的本级 stall，就应该优先记录本级原因。

例如：

- fetch 确实遭遇 `IcacheStall`
- decode 确实发生 `InstMisPred`

此时不应盲目覆盖成下游原因。

### 规则 2：本级只是被下游反压时，转发下游根因

如果本级自身没有新的更直接原因，只是因为下游不再接收而停住，那么应该继续转发下游根因。

这是口径 B 的核心。

### 规则 3：同拍 reason 与同拍 block 一起传播

reverse tick 下，`StallSignals` 已经承担同拍背压的传播职责。

如果 reason 仍然只走延迟 `TimeBuffer`，那么 bool 型 block 和 root cause 就会错拍。

因此：

- 同拍 block 的 root cause 也应通过同拍 sideband 一起传播
- `TimeBuffer` 继续承载延迟控制事件与 payload
- `StallSignals` 则承载同拍背压及其根因元数据

---

## 6. 新版实现中的推荐职责划分

### `dispatchStallReason`

- 最终根因
- 最重要
- 用于真正定位要优化的结构

### `renameStallReason`

- 后端根因向前传播到 rename 的投影

### `decodeStallReason`

- 同一个根因继续传播到 decode 的投影

### `fetchStallReason`

- 该根因最终在 frontend delivery 上体现出的停顿

---

## 7. 修复方向

为了让口径 B 在 reverse tick 下成立，建议做以下修复：

1. 同拍 block 与同拍 root cause 一起传播
2. 提前返回路径也必须发布当前拍的 reason
3. 本级无新原因时，不要留下 stale reason
4. 前级在被 block 时，应直接读取同拍 block root cause，而不是依赖延迟路径的旧值

---

## 8. 新增的结构性背压原因

在这次 reverse tick 统计修复之后，原本容易被吞到 `OtherStall` 里的两类结构性背压，被显式拆成了独立原因：

### `RegFull`

- 含义：rename 本级因为物理寄存器资源不足，无法继续向前推进
- 位置：根因首先发生在 rename
- 传播：如果这个原因继续向前反压，则 `rename/decode/fetch StallReason` 都可以看到 `RegFull`

它不属于“未知 stall”，而是明确的 rename 资源瓶颈。

### `ROBFull`

- 含义：commit 侧因为 ROB 剩余空间不足，无法继续吸收来自 rename 的输入
- 位置：根因首先体现为 commit 对上游的容量背压
- 传播：如果这个原因沿着背压链向前扩散，则 `rename/decode/fetch StallReason` 都可以看到 `ROBFull`

这里的 `ROBFull` 更接近“ROB 容量背压”，而不是“ROB head 对应指令的执行根因”。

因此，这两个 reason 的引入，主要是为了把原本模糊的 `OtherStall` 拆解成更有解释力的结构性原因，而不是改变一级 topdown 行为。

---

## 9. 最后总结

推荐的理解方式是：

- `dispatchStallReason`：最终根因
- `fetch/decode/rename StallReason`：这个根因在更前级造成的影响

因此，修好 root-cause 传播以后：

- `dispatchStallReason` 仍然是最主要的分析依据
- `fetchStallReason` 也会重新变得有参考价值
- 二者不是互相替代，而是互相补充
