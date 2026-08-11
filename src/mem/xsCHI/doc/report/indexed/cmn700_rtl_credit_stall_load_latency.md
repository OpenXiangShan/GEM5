# xsCHI cmn700_rtl credit stall 与 load 生命周期延迟总结

日期：2026-06-01

本文整理当前对话中关于 `cmn700_rtl` 模式下 credit stall、MeshNode queue-like 结构、以及单条 `load` 请求生命周期延迟的结论。目标是统一后续时延对齐、stats 诊断和汇报口径。

## 1. 当前模型的核心语义

### 1.1 已从代码确认

| 概念 | 当前 `cmn700_rtl` 语义 | 默认值 | 代码位置 |
|---|---|---:|---|
| `rxbuf_num` | combined RXBUF 总窗口，表示接收端最多允许 outstanding 的 flit entry 数 | 3 | `configs/common/CacheConfig.py` 参数推导；`src/mem/xsCHI/base/CHIPort.cc::noteRxbufReceive()` / `releaseRxbufEntry()` |
| `skid_depth` | `CHIPort` staging/skid FIFO 诊断深度，不再作为 combined RXBUF 唯一硬上限 | 1 | `src/mem/xsCHI/base/CHIPort.cc::receive()` / `pumpRxbufToStaging()` |
| `ib_depth` | MeshNode ingress/pending 预算，当前由 `MeshNode` admission 控制 | 2 | `src/mem/xsCHI/device/MeshNode.cc::handleIngress()` / `getIngressIbDepth()` |
| `initial_credit_count` | 连接建立时 sender 看到的初始 credit，默认等于 `rxbuf_num` | 3 | `src/mem/xsCHI/base/CHIPort.cc::connect()` 相关初始化 |
| `on_accept` | receiver callback 接受后释放 RXBUF entry，再按配置延迟返还 credit | endpoint port 默认 | `src/mem/xsCHI/base/CHIPort.cc::OnHandleEventCallback()` |
| `on_downstream_release` | MeshNode ingress 接收后不立即返还 credit，等 egress send 成功后释放 | MeshNode port 默认 | `src/mem/xsCHI/device/MeshNode.cc::trySendForOutputAndChannel()` |
| `UpCrdLat` | endpoint -> CMN 方向 credit return latency | `1 + 2 = 3 cycles` | `src/mem/xsCHI/base/CHIPort.cc::creditReturnLatency()` |
| `DnCrdLat` | CMN -> endpoint 方向 credit return latency | `2 + 1 = 3 cycles` | `src/mem/xsCHI/base/CHIPort.cc::creditReturnLatency()` |
| `internal_crd_lat` | MeshNode east/west/north/south 内部链路 credit return latency | 1 cycle | `src/mem/xsCHI/base/CHIPort.py` / `CHIPort.cc::creditReturnLatency()` |

### 1.2 基于结构推断，但仍需用 stats 验证

| 结论 | 说明 | 建议验证项 |
|---|---|---|
| `voq_depth` 在 `cmn700_rtl` 下不应再理解为 CMN `RXBUF_NUM` | 它更接近 MeshNode 内部 pending/IB/仲裁队列容量，不是 endpoint 接口 credit 窗口 | 对比 `ib_full_events`、`egress_credit_blocked_cycles_by_channel`、`rxbuf_outstanding_hist` |
| `skid_depth=1` 不等于只能接收 1 个 flit | combined RXBUF 能接收 `rxbuf_num=3` 个 outstanding；skid 只描述 staging 压力 | 检查 `skid_occupancy_hist` 与 `rxbuf_outstanding_hist` 是否语义分离 |
| credit stall 可以级联传播 | 下游 down/internal blocked 会导致上游 IB/RXBUF entry 不释放，从而继续诱发上游 credit stall | 按 port direction 定位第一个 blocked port |

## 2. Up / Down / Internal credit stall 的定义

`up/down/internal` 描述的是物理连接方向，不是 `REQ/DAT/RSP/SNP` 协议 channel。每个 channel 都有独立 credit 计数，均可能在对应物理方向上发生 stall。

| stall 类型 | 物理方向 | 谁在发送 | 谁的 RXBUF/credit 被占用 | 典型路径 | 默认 credit return latency |
|---|---|---|---|---|---:|
| `up credit stall` | endpoint -> CMN | RN/HN/SN endpoint 往 MeshNode local port 发 | MeshNode local port combined RXBUF | `RNBridge -> MeshNode.local`、`DDRWrapper -> MeshNode.local` | 3 cycles |
| `down credit stall` | CMN -> endpoint | MeshNode local port 往 endpoint 发 | endpoint `networkPort` RXBUF | `MeshNode.local -> RNBridge/HN/DDRWrapper` | 3 cycles |
| `internal credit stall` | CMN mesh 内部 | MeshNode 往 east/west/north/south 发 | 相邻 MeshNode 方向 port RXBUF | `MeshNode(x,y) -> MeshNode(x+1,y)` | 1 cycle |

配置依据：

| 代码位置 | 当前含义 |
|---|---|
| `configs/common/CacheConfig.py::make_chi_port()` | 创建 `CHIPort`，写入 `credit_return_direction` 与 `credit_release_policy` |
| `configs/common/CacheConfig.py::make_mesh_port()` | `cmn700_rtl` 下 MeshNode port 使用 `on_downstream_release` |
| `configs/common/CacheConfig.py` 中 `port_local0=make_mesh_port("up")` | MeshNode local port 接收 endpoint 上传 flit，按 Up credit latency 返回 |
| `configs/common/CacheConfig.py` 中 endpoint `networkPort=make_chi_port("down")` | endpoint 接收 CMN 下载 flit，按 Down credit latency 返回 |
| `configs/common/CacheConfig.py` 中 `port_east/west/north/south=make_mesh_port()` | mesh 内部链路，默认 `internal` credit latency |

## 3. Credit stall 的代码路径

### 3.1 发送侧 credit 检查

发送侧在 `CHIPort::send()` 中检查当前 channel credit：

```text
CHIPort::send(flit)
  -> 根据 flit channel 选择 req/snp/dat/rsp credit
  -> credit == 0 时 recordCreditBlocked()
  -> return false
```

关键代码：

| 文件 | 函数 | 作用 |
|---|---|---|
| `src/mem/xsCHI/base/CHIPort.cc` | `CHIPort::send()` | 每次发送消耗 1 个 channel credit；credit 为 0 时发送失败 |
| `src/mem/xsCHI/device/MeshNode.cc` | `MeshNode::trySendForOutputAndChannel()` | MeshNode egress 调 `egressPort->send(head)`，失败后判断是否 credit blocked |
| `src/mem/xsCHI/device/MeshNode.cc` | `MeshNode::handleCreditUnblock()` | credit 返回后，如果对应 egress/channel 还有 pending flit，则下一周期重试 |

### 3.2 接收侧 release 与 delayed credit return

`cmn700_rtl` 中 MeshNode port 的 credit release 不在接收 callback 成功时发生，而是在 egress send 成功后发生：

```text
sender credit--
  -> receiver CHIPort combined RXBUF outstanding +1
  -> flit 进入 CHIPort rxbuf/staging
  -> MeshNode handleIngress 成功：进入 IB/outVoq
  -> egress send 成功：q.pop()
  -> ingressPort->releaseRxbufEntry(channel, curTick())
  -> delayed credit return event
  -> sender credit++
```

关键代码：

| 文件 | 函数 | 作用 |
|---|---|---|
| `src/mem/xsCHI/base/CHIPort.cc` | `returnCreditToPeer()` | 根据 `creditReturnLatency()` 决定立即 grant 或 enqueue delayed grant |
| `src/mem/xsCHI/base/CHIPort.cc` | `enqueueCreditGrant()` | 将 credit grant event 排到未来 tick |
| `src/mem/xsCHI/base/CHIPort.cc` | `processCreditGrant()` | event 到期后真正 `grantCredit()` |
| `src/mem/xsCHI/base/CHIPort.cc` | `releaseRxbufEntry()` | combined RXBUF entry 真正释放时调用 |
| `src/mem/xsCHI/device/MeshNode.cc` | `trySendForOutputAndChannel()` | egress send 成功后调用入口 port 的 `releaseRxbufEntry()` |

## 4. 三类 hop 的直观时序口径

当前讨论中保留以下口径：

| 情况 | 最小直观延迟 | 说明 |
|---|---:|---|
| baseline mesh hop | 2 cycles | 当前 MeshNode/CHIPort port transfer 与处理口径下的无 stall 单 hop |
| internal credit stall hop | 至少 4 cycles | `2 + internal_crd_lat(1) + retry_schedule(1)` |
| endpoint-facing credit stall hop | 至少 6 cycles | `2 + Up/DnCrdLat(3) + retry_schedule(1)` |

### 4.1 baseline hop：2 cycles

```text
cycle 0: sender egress send 成功，credit--
cycle 1: receiver port transfer / handle event 等待
cycle 2: receiver callback 处理，flit 可进入下一阶段
```

### 4.2 internal credit stall hop：至少 4 cycles

```text
cycle 0: MeshNode A 准备发往 MeshNode B，internal credit == 0，send 失败
cycle 1: 等待 internal credit return
cycle 2: credit grant 可见，调度下一周期 retry
cycle 3: retry send 成功
cycle 4: receiver 侧完成 baseline hop 接收处理
```

简化公式：

```text
T_internal_stall_hop >= T_baseline_hop + internal_crd_lat + T_retry
                     >= 2 + 1 + 1
                     >= 4 cycles
```

### 4.3 endpoint-facing credit stall hop：至少 6 cycles

```text
cycle 0: endpoint-facing send 失败，Up/Dn credit == 0
cycle 1: 等待 credit return
cycle 2: 等待 credit return
cycle 3: Up/Dn credit grant 到达
cycle 4: 调度 retry
cycle 5: retry send 成功
cycle 6: receiver 侧完成 baseline hop 接收处理
```

简化公式：

```text
T_endpoint_stall_hop >= T_baseline_hop + UpOrDnCrdLat + T_retry
                     >= 2 + 3 + 1
                     >= 6 cycles
```

## 5. Stall 是否会叠加

### 5.1 同一个 egress port 的同一次 send 不会同时叠加

一个 `CHIPort` 的 `credit_return_direction` 固定为 `up/down/internal` 之一。因此同一个 flit 在同一个 egress port 的同一次 `send()` 失败，只能归因到一种 credit stall。

错误理解：

```text
一个 internal hop stall = 2 + internal_crd_lat + up_crd_lat + down_crd_lat
```

正确理解：

```text
internal hop stall        = 2 + internal_crd_lat + retry
endpoint-facing hop stall = 2 + up_or_down_crd_lat + retry
```

### 5.2 跨 hop、跨事务阶段会叠加

同一个 flit 可能先经过 internal hop，再进入 endpoint-facing hop；完整事务也可能由多个 flit 组成。因此 end-to-end latency 上会累加。

示例：

```text
RN endpoint
  -> MeshNode(0,0).local       : Up, 可能 up credit stall
  -> MeshNode(0,1).internal    : Internal, 可能 internal credit stall
  -> HN endpoint               : Down, 可能 down credit stall
```

如果三段都 stall，最小直观延迟：

```text
T >= 6 + 4 + 6 = 16 cycles
```

如果三段都不 stall：

```text
T = 2 + 2 + 2 = 6 cycles
```

额外 stall 代价：

```text
extra >= (6 - 2) + (4 - 2) + (6 - 2) = 10 cycles
```

### 5.3 Backpressure 传播会产生级联 stall

典型传播链：

```text
down credit stall
  -> MeshNode local egress 发不出去
  -> flit 留在 IB/outVoq
  -> ingressPort->releaseRxbufEntry() 不发生
  -> 上游 sender 的 credit 不返回
  -> 上游 internal/up credit stall
```

因此，如果 stats 中同时出现 downstream endpoint blocked 和 upstream internal blocked，不能简单认为重复计数，而应判断哪个是原发阻塞、哪个是 backpressure 传播结果。

## 6. Load 请求完整生命周期

以下以用户给定拓扑为例：

| 角色 | 坐标 |
|---|---|
| RN | `node(0,0)` |
| HN | `node(0,1)` |
| SN | `node(1,0)` |

### 6.1 Load hit at HN 的路径

假设 RN 发出 cacheable load，HN/SLC 命中并直接返回数据：

```text
REQ:
RN endpoint
  -> MeshNode(0,0).local    [Up]
  -> MeshNode(0,1)          [Internal, 1 mesh hop]
  -> HN endpoint            [Down]

HN local lookup / response preparation

DAT:
HN endpoint
  -> MeshNode(0,1).local    [Up]
  -> MeshNode(0,0)          [Internal, 1 mesh hop]
  -> RN endpoint            [Down]
```

分阶段延迟公式：

```text
T_load_hit =
  T_up(RN -> Mesh00, REQ)
+ T_internal(Mesh00 -> Mesh01, REQ)
+ T_down(Mesh01 -> HN, REQ)
+ T_HN_lookup
+ T_up(HN -> Mesh01, DAT)
+ T_internal(Mesh01 -> Mesh00, DAT)
+ T_down(Mesh00 -> RN, DAT)
+ T_RN_fill_or_wakeup
```

理想无 stall、忽略 HN/RN 本地处理时：

```text
T_load_hit_mesh_only_min = 6 hops/segments * 2 cycles = 12 cycles
```

完整口径应保留：

```text
T_load_hit =
  12
+ T_HN_lookup
+ T_RN_fill_or_wakeup
+ T_queue
+ T_credit_stall
+ T_arbitration
```

### 6.2 Load miss to SN 的路径

假设 HN miss，需要访问 SN/DDR：

```text
REQ 1:
RN endpoint
  -> MeshNode(0,0).local    [Up]
  -> MeshNode(0,1)          [Internal, 1 mesh hop]
  -> HN endpoint            [Down]

HN miss / memory request preparation

REQ 2:
HN endpoint
  -> MeshNode(0,1).local    [Up]
  -> MeshNode(1,1)          [Internal]
  -> MeshNode(1,0)          [Internal]
  -> SN endpoint            [Down]

SN/DDR memory access

DAT 1:
SN endpoint
  -> MeshNode(1,0).local    [Up]
  -> MeshNode(1,1)          [Internal]
  -> MeshNode(0,1)          [Internal]
  -> HN endpoint            [Down]

HN data response / possible state update

DAT 2:
HN endpoint
  -> MeshNode(0,1).local    [Up]
  -> MeshNode(0,0)          [Internal, 1 mesh hop]
  -> RN endpoint            [Down]
```

这里 HN `(0,1)` 到 SN `(1,0)` 按 XY/mesh 路由需要 2 个 internal mesh hop。若实际路由策略不同，应以 `MeshNode::routeFor()` 和 hop count stats 为准。

分阶段延迟公式：

```text
T_load_miss =
  T_RN_to_HN_REQ
+ T_HN_lookup_miss
+ T_HN_to_SN_REQ
+ T_SN_mem_access
+ T_SN_to_HN_DAT
+ T_HN_data_process
+ T_HN_to_RN_DAT
+ T_RN_fill_or_wakeup
```

展开为方向/credit 口径：

```text
T_RN_to_HN_REQ =
  T_up_REQ(RN -> Mesh00)
+ T_internal_REQ(Mesh00 -> Mesh01)
+ T_down_REQ(Mesh01 -> HN)

T_HN_to_SN_REQ =
  T_up_REQ(HN -> Mesh01)
+ T_internal_REQ(Mesh01 -> Mesh11)
+ T_internal_REQ(Mesh11 -> Mesh10)
+ T_down_REQ(Mesh10 -> SN)

T_SN_to_HN_DAT =
  T_up_DAT(SN -> Mesh10)
+ T_internal_DAT(Mesh10 -> Mesh11)
+ T_internal_DAT(Mesh11 -> Mesh01)
+ T_down_DAT(Mesh01 -> HN)

T_HN_to_RN_DAT =
  T_up_DAT(HN -> Mesh01)
+ T_internal_DAT(Mesh01 -> Mesh00)
+ T_down_DAT(Mesh00 -> RN)
```

理想无 stall、忽略 HN/SN/RN 本地处理时，mesh/endpoint-facing segment 数量为：

| 阶段 | Up | Internal | Down | segment 数 |
|---|---:|---:|---:|---:|
| RN -> HN REQ | 1 | 1 | 1 | 3 |
| HN -> SN REQ | 1 | 2 | 1 | 4 |
| SN -> HN DAT | 1 | 2 | 1 | 4 |
| HN -> RN DAT | 1 | 1 | 1 | 3 |
| 合计 | 4 | 6 | 4 | 14 |

因此：

```text
T_load_miss_mesh_only_min = 14 * 2 = 28 cycles
```

完整口径：

```text
T_load_miss =
  28
+ T_HN_lookup_miss
+ T_HN_mem_req_build
+ T_SN_mem_access
+ T_HN_data_process
+ T_RN_fill_or_wakeup
+ T_queue
+ T_credit_stall
+ T_arbitration
```

其中：

```text
T_credit_stall =
  Σ T_up_credit_stall
+ Σ T_down_credit_stall
+ Σ T_internal_credit_stall
```

## 7. Queue-like 延迟如何进入公式

在 `cmn700_rtl` 中，不应只计算 hop latency。XP/mesh 内部 queue-like 结构会通过以下方式进入总延迟：

| 项 | 来源 | 何时为 0 | 何时增加 |
|---|---|---|---|
| `T_skid_wait` | `CHIPort` staging/skid | staging 可及时推进 | IB full 或 callback 无法接受 |
| `T_IB_wait` | MeshNode ingress/pending | IB 有空且 egress 很快可发 | 下游 blocked、仲裁未选中、same egress/channel 堆积 |
| `T_arbitration` | MeshNode round-robin egress arbitration | 单 flit、无竞争 | 多 ingress 竞争同一 egress/channel |
| `T_credit_stall` | `CHIPort::send()` credit 为 0 | credit 充足 | RXBUF entry 未释放或 credit return latency 尚未到期 |
| `T_retry_schedule` | credit unblock 后下一周期调度重试 | 无 stall | 发生 credit blocked 后至少多 1 cycle |

代码对应：

| 延迟项 | 代码位置 |
|---|---|
| skid/rxbuf receive | `src/mem/xsCHI/base/CHIPort.cc::receive()` |
| rxbuf -> staging 推进 | `src/mem/xsCHI/base/CHIPort.cc::pumpRxbufToStaging()` |
| MeshNode ingress admission | `src/mem/xsCHI/device/MeshNode.cc::handleIngress()` |
| egress arbitration | `src/mem/xsCHI/device/MeshNode.cc::trySendForOutputAndChannel()` |
| credit blocked 判断 | `src/mem/xsCHI/base/CHIPort.cc::send()` |
| credit unblock retry | `src/mem/xsCHI/device/MeshNode.cc::handleCreditUnblock()` |

## 8. Stats 诊断建议

### 8.1 判断 stall 类型

| 观察点 | 可能含义 |
|---|---|
| MeshNode east/west/north/south egress blocked | internal credit stall |
| MeshNode local egress 发 endpoint blocked | down credit stall |
| endpoint 往 MeshNode local 发不出去 | up credit stall |
| `rxbuf_outstanding` 长时间接近 `rxbuf_num` | combined RXBUF entry 没有及时释放 |
| `skid_occupancy` 升高 | staging 压力增大，可能 IB 或 callback 推进不及时 |
| `ib_full_events` 升高 | MeshNode ingress/pending 预算不足 |
| `egress_credit_blocked_cycles_by_channel` 升高 | egress 下游 credit 不足 |

### 8.2 推荐排查顺序

1. 先定位第一个出现 credit blocked 的 port。
2. 判断该 port 的 `credit_return_direction` 是 `up/down/internal`。
3. 检查该 port 下游是否有 IB/outVoq 堆积。
4. 检查上游 blocked 是否由下游 backpressure 传播引起。
5. 分 channel 看 `REQ/DAT/RSP/SNP`，不要把 channel 和 Up/Dn 方向混为一谈。

## 9. 关键注意事项

| 风险 | 正确处理方式 |
|---|---|
| 把 `credit return latency` 当成 flit 数据前向 hop latency | 两者分开：baseline hop 表示数据前向，credit latency 表示接收窗口释放后的 credit 回环 |
| 把 `Up/Dn` 当成 `REQ/DAT` | Up/Dn 是物理方向，REQ/DAT/RSP/SNP 是协议 channel |
| 把 `voq_depth` 当成 `RXBUF_NUM` | `RXBUF_NUM` 是接口 receive window；VOQ/IB 是 mesh 内部 pending/仲裁队列 |
| 同一个 hop 上叠加 internal 和 endpoint credit latency | 同一个 egress port 只有一种 direction；只能跨 hop/跨阶段叠加 |
| 只看总 latency histogram | 需要结合 credit、IB、skid、egress blocked stats 才能归因 |
| `rxbuf_num=3` 被理解成永不 stall | 它只保证理想连续流窗口大小；下游堵塞或仲裁竞争仍会导致 stall |

## 10. 汇报口径简版

当前 `cmn700_rtl` 模型把 xsCHI 的链路时序拆成了 endpoint-facing Up/Dn credit、mesh internal credit、combined RXBUF、skid staging、MeshNode IB/pending 和 egress arbitration。理想情况下单 hop 按 2 cycles 计算；发生 internal credit stall 时至少变为 4 cycles；发生 endpoint-facing up/down credit stall 时至少变为 6 cycles。三类 stall 不会在同一个 egress port 的一次发送上重复叠加，但会在完整 load 路径和 backpressure 传播中按阶段累加。对 RN(0,0)、HN(0,1)、SN(1,0) 的 load miss 路径，忽略本地 HN/SN/RN 处理和所有非理想等待时，mesh/endpoint-facing segment 合计 14 段，最小链路延迟约为 28 cycles；完整延迟还需要叠加 HN lookup、SN memory access、RN fill、IB/skid/仲裁和 credit stall 等项。
