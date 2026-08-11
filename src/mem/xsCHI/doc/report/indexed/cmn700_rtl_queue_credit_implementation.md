# xsCHI cmn700_rtl Queue/Credit 实现说明

本文档说明当前 xsCHI 新增的 `cmn700_rtl` credit/queue 对齐机制，重点解释它如何更贴近 CMN-700 RTL 中的接口接收窗口、skid buffer、IB/pending queue 与 credit return 语义。

本文只描述已落地的代码实现，不描述尚未实现的 RTL 细节。

## 1. 核心结论

| 项 | 当前实现 |
|---|---|
| 新模式 | `credit_model="cmn700_rtl"`，与 `legacy`、`cmn700` 并存 |
| RXBUF 语义 | `rxbuf_num` 表示 combined RXBUF 总预算 |
| skid 语义 | `CHIPort` per-channel staging FIFO 深度；RTL MeshNode 端口通过 `rxbuf_queue -> staging` pump 实现，staging 满时 flit 留在 RXBUF |
| IB 语义 | `MeshNode::outVoq` 在 `cmn700_rtl` 下承担 ingress/channel 维度的 IB/pending slot 语义 |
| 默认窗口 | `rxbuf_num=3, skid_depth=1, ib_depth=2` |
| 初始 credit | `initial_credit_count=rxbuf_num`，默认 3 |
| credit release 起点 | MeshNode-owned port 在 egress send 成功后释放 combined RXBUF entry |
| credit return 延迟 | 复用已有 delayed credit return event queue，按 `up/down/internal` 配置延迟返还 |
| endpoint 行为 | endpoint port 不建 IB，callback accept 后立即 release combined RXBUF entry，再进入 delayed credit return |

## 2. CMN-700 RTL 语义到 xsCHI 的映射

| CMN-700 RTL 概念 | xsCHI 实现位置 | 当前语义 |
|---|---|---|
| `RXBUF_NUM` | [CHIPort.py](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.py:12), [CHIPort.hh](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:131) | combined receive window 总预算 |
| skid buffer | [CHIPort.py](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.py:14), [CHIPort.hh](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:139) | `CHIPort` 内 per-channel staging FIFO 深度 |
| IB / pending slot | [MeshNode.py](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.py:15), [MeshNode.hh](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.hh:91) | MeshNode 内部 ingress/channel pending 容量 |
| `RXBUF = skid + IB` | [CacheConfig.py](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:501) | 配置层校验 `rxbuf_num == skid_depth + ib_depth` |
| 每发 1 flit 消耗 1 credit | [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:640) | `send()` 中按 channel 检查并递减 credit |
| credit release | [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:601), [MeshNode.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:516) | combined RXBUF entry 释放后才触发返还 |
| credit return latency | [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:346), [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:398) | `releaseTick -> grantTick` delayed event |

## 3. 参数入口与默认值

### 3.1 SimObject 参数

`CHIPort` 新增和扩展的参数位于 [CHIPort.py](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.py:9)：

| 参数 | 代码位置 | 语义 |
|---|---|---|
| `recv_buffer_size` | [CHIPort.py:11](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.py:11) | legacy alias，`0` 表示自动 |
| `rxbuf_num` | [CHIPort.py:12](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.py:12) | CMN-style receive flit buffer entries |
| `skid_depth` | [CHIPort.py:14](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.py:14) | CMN RTL-style skid/staging entries |
| `initial_credit_count` | [CHIPort.py:16](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.py:16) | 初始 advertised credits |
| `credit_model` | [CHIPort.py:19](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.py:19) | `legacy/cmn700/cmn700_rtl` |
| `credit_return_direction` | [CHIPort.py:21](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.py:21) | `up/down/internal` |
| `credit_release_policy` | [CHIPort.py:23](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.py:23) | `on_accept/on_downstream_release` |

`MeshNode` 新增 `ib_depth` 位于 [MeshNode.py](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.py:15)：

| 参数 | 代码位置 | 语义 |
|---|---|---|
| `voq_depth` | [MeshNode.py:14](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.py:14) | legacy/current cmn700 VOQ 阈值 |
| `ib_depth` | [MeshNode.py:15](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.py:15) | cmn700_rtl 下 ingress buffer 深度，`0` 使用 `voq_depth/default` |

### 3.2 CLI 参数

命令行参数位于 [Options.py](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/Options.py:150)：

| CLI | 代码位置 | 用途 |
|---|---|---|
| `--chi-credit-model` | [Options.py:150](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/Options.py:150) | 选择 `legacy/cmn700/cmn700_rtl` |
| `--chi-rxbuf-num` | [Options.py:157](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/Options.py:157) | 指定 combined RXBUF |
| `--chi-skid-depth` | [Options.py:161](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/Options.py:161) | 指定 skid/staging 深度 |
| `--chi-ib-depth` | [Options.py:164](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/Options.py:164) | 指定 MeshNode IB 深度 |
| `--chi-initial-credit-count` | [Options.py:167](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/Options.py:167) | 指定初始 credit |

### 3.3 配置层推导与校验

`cmn700_rtl` 的默认和合法性约束主要在 [CacheConfig.py](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:501) 完成：

| 规则 | 代码位置 | 行为 |
|---|---|---|
| `rxbuf_num=0` 时默认 3 | [CacheConfig.py:501-503](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:501) | 对齐 CMN 默认 `RXBUF_NUM=3` |
| skid 和 IB 都未配置 | [CacheConfig.py:504-506](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:504) | 默认 `ib_depth=2, skid_depth=rxbuf-ib=1` |
| 只配置 skid | [CacheConfig.py:507-508](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:507) | 反推 `ib_depth=rxbuf-skid` |
| 只配置 IB | [CacheConfig.py:509-510](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:509) | 反推 `skid_depth=rxbuf-ib` |
| 三者校验 | [CacheConfig.py:511-516](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:511) | 要求 `rxbuf_num == skid_depth + ib_depth` |
| skid/IB 必须为正 | [CacheConfig.py:517-520](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:517) | 避免无效窗口 |
| initial credit 默认 | [CacheConfig.py:522-523](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:522) | `initial_credit_count=rxbuf_num` |
| initial credit 上界 | [CacheConfig.py:524-528](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:524) | 当前不允许 `initial_credit_count > rxbuf_num` |

对应的 C++ 侧也有防御式检查，位于 [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:293)：

| 检查 | 代码位置 | 说明 |
|---|---|---|
| credit model 合法性 | [CHIPort.cc:293-296](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:293) | 只允许 `legacy/cmn700/cmn700_rtl` |
| `rxbufNum > 0` | [CHIPort.cc:297-298](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:297) | 防止无接收窗口 |
| `skidDepth > 0` | [CHIPort.cc:299-300](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:299) | 防止 staging FIFO 无效 |
| `initialCreditCount > 0` | [CHIPort.cc:301-303](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:301) | 防止连接后不可发送 |
| `initialCreditCount <= rxbufNum` | [CHIPort.cc:304-306](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:304) | 当前不建模 LL credit > RXBUF |
| `skidDepth <= rxbufNum` | [CHIPort.cc:307-309](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:307) | 防止 skid 大于总窗口 |

## 4. CHIPort 内部状态拆分

`CHIPort` 的关键状态位于 [CHIPort.hh](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:131)：

| 字段 | 代码位置 | 语义 |
|---|---|---|
| `rxbufNum` | [CHIPort.hh:131](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:131) | combined RXBUF 总预算 |
| `skidDepth` | [CHIPort.hh:139](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:139) | RTL-like skid/staging FIFO 深度 |
| `initialCreditCount` | [CHIPort.hh:133](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:133) | peer sender 初始 credit |
| `delayedCreditReturn` | [CHIPort.hh:134](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:134) | 是否启用 delayed credit return |
| `rtlCreditModel` | [CHIPort.hh:135](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:135) | 是否为 `cmn700_rtl` |
| `creditReturnDirection` | [CHIPort.hh:136](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:136) | Up/Dn/Internal |
| `creditReleasePolicy` | [CHIPort.hh:137](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:137) | on accept 或 downstream release |
| `rxbufOutstanding` | [CHIPort.hh:143](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:143) | per-channel 已占用但尚未 release 的 combined RXBUF entry |
| `req/snp/dat/rsp_rxbuf` | [CHIPort.hh:97-102](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:97) | `cmn700_rtl` MeshNode 端口的 per-channel RXBUF queue |
| `req/snp/dat/rsp_buffer` | [CHIPort.hh:84-95](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:84) | callback 消费的 per-channel staging/skid FIFO |

`rxbufOutstanding` 是 `cmn700_rtl` 的核心 accounting 状态。它不是 `CHIPort` RXBUF queue 当前深度，也不是 staging FIFO 当前深度，而是“这个接收端已经消耗了多少 combined RXBUF credit window”。当 flit 从 `CHIPort` RXBUF queue 搬入 staging，再进入 `MeshNode` IB 后，`rxbufOutstanding` 仍保持占用，直到 MeshNode egress send 成功后才释放。

## 5. 发送侧 credit 消耗

发送侧逻辑仍在 [CHIPort::send()](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:640) 中实现。

| channel | 检查与消耗位置 | 行为 |
|---|---|---|
| REQ | [CHIPort.cc:646-659](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:646) | `req_credit==0` 时失败；成功时 `req_credit--` |
| SNP | [CHIPort.cc:661-674](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:661) | `snp_credit==0` 时失败；成功时 `snp_credit--` |
| DAT | [CHIPort.cc:676-689](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:676) | `dat_credit==0` 时失败；成功时 `dat_credit--` |
| RSP | [CHIPort.cc:691-704](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:691) | `rsp_credit==0` 时失败；成功时 `rsp_credit--` |

credit 不足时会调用 `recordCreditBlocked()`，统计点位于 [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:462)：

| 统计 | 代码位置 | 含义 |
|---|---|---|
| `credit_stall_events_by_channel` | [CHIPort.cc:466-468](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:466) | 第一次因 credit 不足进入 blocked |
| `no_credit_bubble_cycles_by_channel` | [CHIPort.cc:473-478](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:473) | 记录无 credit bubble cycle |

初始 credit 在连接时由 peer 的 `initialCreditCount` 决定，位置是 [CHIPort::connect()](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:1053)：

| 行为 | 代码位置 |
|---|---|
| 本端 sender credit 初始化为 peer advertised credit | [CHIPort.cc:1059-1062](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:1059) |
| peer sender credit 初始化为本端 advertised credit | [CHIPort.cc:1070-1073](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:1070) |

这对应 CMN credit-based flow control 的基本语义：sender 每发送 1 个 flit 消耗 1 个 receiver-advertised credit。

## 6. 接收侧 RXBUF、staging/skid 与 combined RXBUF accounting

接收入口是 [CHIPort::receive()](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:720)。

关键逻辑：

| 步骤 | 代码位置 | 行为 |
|---|---|---|
| RTL MeshNode 端口识别 | [CHIPort.cc:626-630](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:626) | `rtlCreditModel && on_downstream_release` 使用 RXBUF queue + staging |
| combined RXBUF 占用 +1 | [CHIPort.cc:820-824](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:820) | RTL MeshNode 端口先调用 `noteRxbufReceive()`，再 push 到 `rxbufQueue()` |
| 非 RTL 接收 | [CHIPort.cc:825-834](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:825) | legacy/current cmn700/endpoint 仍直接 push 到 callback staging FIFO |
| RXBUF -> staging pump | [CHIPort.cc:632-648](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:632) | 只要 staging 深度小于 `skidDepth`，就从 RXBUF queue 搬入 staging |
| callback 事件入口 | [CHIPort.cc:839-861](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:839) | 每次事件先 pump，再按 SNP/RSP/DAT/REQ 处理 staging |
| staging/RXBUF 有剩余时重调度 | [CHIPort.cc:857-860](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:857) | `hasReceiveWork()` 会同时检查 staging FIFO 和 RTL RXBUF queue |

`noteRxbufReceive()` 位于 [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:585)：

| 行为 | 代码位置 | 说明 |
|---|---|---|
| 非 RTL 模式直接返回 | [CHIPort.cc:588-590](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:588) | legacy/cmn700 不做 combined accounting |
| outstanding 溢出检查 | [CHIPort.cc:592-596](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:592) | 不允许超过 `rxbufNum` |
| `rxbufOutstanding++` | [CHIPort.cc:597](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:597) | 接收 1 flit 占用 1 combined RXBUF entry |
| 采样 outstanding | [CHIPort.cc:598](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:598) | 用于验证 `RXBUF = skid + IB` |

这一段是 `cmn700_rtl` 相比旧实现的关键变化：接收入口不再直接把 flit 塞进 callback staging FIFO，而是先进入 per-channel RXBUF queue。`skidDepth=1` 只限制 RXBUF 到 callback staging 的推进窗口；如果 staging 满，后续 flit 保留在 RXBUF queue 中等待下一次事件，不会因为 staging 满而 panic。真正的总窗口仍由 `rxbufOutstanding < rxbufNum` 约束。

## 7. callback accept 后的两种 release policy

`CHIPort` 每周期按 SNP/RSP/DAT/REQ 顺序处理 staging FIFO，调度入口是 [CHIPort::OnHandleEventCallback()](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:740)。

每个 channel 的处理逻辑结构一致：

| channel | callback 成功后处理位置 | release 行为 |
|---|---|---|
| REQ | [CHIPort.cc:763-783](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:763) | `on_accept` 立即 `releaseRxbufEntry()`；否则只记录 deferred |
| SNP | [CHIPort.cc:803-823](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:803) | 同上 |
| DAT | [CHIPort.cc:843-863](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:843) | 同上 |
| RSP | [CHIPort.cc:883-903](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:883) | 同上 |

以 REQ 为例：

| 行为 | 代码位置 | 说明 |
|---|---|---|
| callback 成功 | [CHIPort.cc:770](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:770) | 下游接受 flit |
| staging FIFO pop | [CHIPort.cc:772](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:772) | flit 离开 skid/staging |
| `on_accept` 立即 release | [CHIPort.cc:777-779](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:777) | endpoint 或 legacy/current cmn700 行为 |
| `on_downstream_release` 延后 release | [CHIPort.cc:780-782](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:780) | MeshNode-owned port 的 RTL 行为 |
| callback reject 计数 | [CHIPort.cc:785-790](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:785) | 下游不能接收时 flit 留在 staging |

`credit_release_policy` 的判断函数在 [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:579)：

| 函数 | 代码位置 | 含义 |
|---|---|---|
| `releaseCreditOnAccept()` | [CHIPort.cc:579-583](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:579) | 是否 callback accept 后立即释放 credit |

## 8. MeshNode IB/pending 行为

### 8.1 MeshNode 初始化

`MeshNode` 中 `ibDepth` 初始化位于 [MeshNode.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:232)：

| 字段 | 代码位置 | 行为 |
|---|---|---|
| `voqDepth` | [MeshNode.cc:238](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:238) | legacy/current cmn700 VOQ 限制 |
| `ibDepth` | [MeshNode.cc:239-240](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:239) | `ib_depth=0` 时 fallback 到 `voq_depth` 或默认 2 |
| `ibDepth` 合法性 | [MeshNode.cc:249](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:249) | 不允许 0 |

`outVoq` 的数据结构在 [MeshNode.hh](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.hh:83)：

| 类型 | 代码位置 | 语义 |
|---|---|---|
| `OutputQueues` | [MeshNode.hh:83-86](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.hh:83) | `[egress][channel][ingress]` queue |
| `outVoq` | [MeshNode.hh:95-97](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.hh:95) | 在 RTL 模式下承担 IB/pending slot |

### 8.2 ingress admission

MeshNode ingress callback 是 [MeshNode::handleIngress()](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:307)。

关键逻辑：

| 步骤 | 代码位置 | 行为 |
|---|---|---|
| route 选择 egress | [MeshNode.cc:312](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:312) | 进入目标输出方向 |
| 判断是否 RTL IB 模式 | [MeshNode.cc:330-331](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:330) | ingress port 若 `on_downstream_release`，则使用 IB admission |
| 计算 selected depth | [MeshNode.cc:332-337](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:332) | RTL 模式按 ingress/channel 总 IB occupancy，非 RTL 按 VOQ depth |
| 选择 depth limit | [MeshNode.cc:338](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:338) | RTL 用 `ibDepth`，非 RTL 用 `voqDepth` |
| IB/VOQ full backpressure | [MeshNode.cc:340-363](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:340) | 返回 false，flit 留在 upstream CHIPort staging |
| 入队 outVoq | [MeshNode.cc:366-367](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:366) | flit 从 CHIPort staging 进入 MeshNode IB/pending |
| 调度 send event | [MeshNode.cc:377](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:377) | 尝试从 IB 发往 egress |

RTL 模式的 IB occupancy 不是单一 egress queue 深度，而是同一 ingress/channel 分散到所有 egress 的合计深度。实现位于 [MeshNode::getIngressIbDepth()](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:607)：

| 行为 | 代码位置 |
|---|---|
| 遍历所有 egress | [MeshNode.cc:614-617](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:614) |
| 返回 ingress/channel 总占用 | [MeshNode.cc:618](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:618) |
| `canAllocateIbSlot()` 判断 `< ibDepth` | [MeshNode.cc:621-625](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:621) |

这使 `ib_depth` 表示“该 ingress/channel 已经进入 MeshNode 的 pending entries 总数”，而不是某个 egress 的局部 VOQ 深度。

### 8.3 egress send 成功后释放 combined RXBUF

MeshNode egress arbitration 和发送在 [MeshNode::trySendForOutputAndChannel()](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:447)。

关键点：

| 步骤 | 代码位置 | 行为 |
|---|---|---|
| 选择 egress/channel 下的 ingress source | [MeshNode.cc:461-468](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:461) | round-robin |
| 尝试通过 egress port 发送 | [MeshNode.cc:490](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:490) | 调用 `egressPort->send(head)` |
| send 成功后 pop IB/pending | [MeshNode.cc:516](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:516) | flit 离开 `outVoq` |
| 如果 ingress port 是 downstream-release，则释放 RXBUF | [MeshNode.cc:517-520](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:517) | 调用 `ingressPort->releaseRxbufEntry(channel, curTick())` |
| egress credit blocked 统计 | [MeshNode.cc:525-528](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.cc:525) | 区分下游 credit 造成的阻塞 |

这段逻辑是 `cmn700_rtl` 的 credit release 语义核心：flit 进入 MeshNode IB 后不会立刻返还 upstream credit，只有当它被成功送出 egress、从 IB/pending 中释放时，才 release combined RXBUF entry。

## 9. delayed credit return event queue

### 9.1 latency 选择

credit return latency 在 [CHIPort::creditReturnLatency()](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:346)：

| direction | 代码位置 | 公式 |
|---|---|---|
| legacy | [CHIPort.cc:349-350](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:349) | `0` |
| Up | [CHIPort.cc:353-355](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:353) | `upCrdLatInt + upCrdLatExt` |
| Down | [CHIPort.cc:356-357](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:356) | `dnCrdLatInt + dnCrdLatExt` |
| Internal | [CHIPort.cc:358-359](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:358) | `internalCrdLat` |

`cmn700` 和 `cmn700_rtl` 都通过 `isDelayedCreditModel()` 启用 delayed return，判断位于 [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:59)：

| 函数 | 代码位置 | 行为 |
|---|---|---|
| `isCmn700RtlCreditModel()` | [CHIPort.cc:53-57](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:53) | 判断 `model == "cmn700_rtl"` |
| `isDelayedCreditModel()` | [CHIPort.cc:59-63](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:59) | `cmn700` 或 `cmn700_rtl` 返回 true |

### 9.2 release 到 grant 的事件链

`releaseRxbufEntry()` 位于 [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:601)：

| 行为 | 代码位置 | 说明 |
|---|---|---|
| RTL 模式 outstanding-- | [CHIPort.cc:604-610](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:604) | combined RXBUF entry 真正释放 |
| 统计 release event | [CHIPort.cc:613](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:613) | `rxbuf_release_events_by_channel++` |
| 进入 credit return | [CHIPort.cc:614](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:614) | 调用 `returnCreditToPeer(channel, releaseTick)` |

`returnCreditToPeer()` 位于 [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:398)：

| 情况 | 代码位置 | 行为 |
|---|---|---|
| latency 为 0 | [CHIPort.cc:401-405](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:401) | 立即在 peer 上 `grantCredit()` |
| latency 大于 0 | [CHIPort.cc:407](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:407) | 在 peer 上 `enqueueCreditGrant()` |

`enqueueCreditGrant()` 位于 [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:410)：

| 行为 | 代码位置 |
|---|---|
| 计算 `grantTick = curTick() + clockPeriod() * latency` | [CHIPort.cc:416](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:416) |
| push 到 per-channel credit grant queue | [CHIPort.cc:417](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:417) |
| 若 event 未调度则 schedule | [CHIPort.cc:418-420](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:418) |

`processCreditGrant()` 位于 [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:423)：

| 行为 | 代码位置 |
|---|---|
| 到期时 pop pending credit return | [CHIPort.cc:429-431](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:429) |
| 采样 return latency | [CHIPort.cc:432](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:432) |
| 真正 `grantCredit()` | [CHIPort.cc:433](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:433) |
| 队列还有 event 时继续 schedule | [CHIPort.cc:436-438](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:436) |

`grantCredit()` 按 channel 分发到 `GrantCredit_REQ/SNP/DAT/RSP`，入口位于 [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:441)。以 REQ 为例，credit 增加和 unblock 统计位于 [CHIPort.cc](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:922)：

| 行为 | 代码位置 |
|---|---|
| `req_credit++` | [CHIPort.cc:923-925](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:923) |
| 统计 blocked cycles | [CHIPort.cc:932-941](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:932) |
| 触发 unblock callback | [CHIPort.cc:942-947](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.cc:942) |

## 10. 配置层 release policy：MeshNode 与 endpoint 区分

`make_chi_port()` 和 `make_mesh_port()` 位于 [CacheConfig.py](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:537)。

| 函数 | 代码位置 | 行为 |
|---|---|---|
| `make_chi_port()` | [CacheConfig.py:537-567](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:537) | 创建普通 `CHIPort`，默认 `on_accept` |
| `make_mesh_port()` | [CacheConfig.py:569-573](/nfs/home/wuchengkai/GEM5_reps/GEM5/configs/common/CacheConfig.py:569) | `cmn700_rtl` 下 MeshNode port 使用 `on_downstream_release` |

当前配置策略：

| port 类型 | release policy | 语义 |
|---|---|---|
| MeshNode local/east/west/north/south port | `cmn700_rtl` 下为 `on_downstream_release` | flit 进入 MeshNode IB 后不返 credit，egress send 成功后释放 |
| endpoint/CHIBridge/L3/DDRWrapper 等普通 port | `on_accept` | callback 接收成功后释放 |
| `legacy/cmn700` | `on_accept` | 保持旧行为或 current cmn700 delayed credit 行为 |

这一区分很关键：Up/Dn/Internal 是物理 credit return latency 方向，REQ/SNP/DAT/RSP 是 CHI channel，`credit_release_policy` 是接收窗口释放时机，三者不是同一个维度。

## 11. 单 flit 生命周期

### 11.1 MeshNode-owned receiver port，`cmn700_rtl`

```text
sender CHIPort::send()
  credit--
  connected_port->receive()

receiver MeshNode CHIPort::receive()
  rxbufOutstanding++
  push into CHIPort per-channel RXBUF queue
  schedule OnHandleEventCallback after PortTransferLatency

CHIPort::OnHandleEventCallback()
  pump RXBUF queue -> staging FIFO while staging.size < skidDepth

CHIPort::OnHandleEventCallback_<CH>()
  receive_callback(head) -> MeshNode::handleIngress()
  if IB has slot:
    pop CHIPort staging FIFO
    do not release credit
    deferred_credit_release_events++
  else:
    callback returns false
    flit remains in staging FIFO
    credit is not released

MeshNode::handleIngress()
  push flit into outVoq[egress][channel][ingress]

MeshNode::trySendForOutputAndChannel()
  if downstream egressPort->send(head) succeeds:
    pop outVoq entry
    ingressPort->releaseRxbufEntry(channel, curTick())

CHIPort::releaseRxbufEntry()
  rxbufOutstanding--
  returnCreditToPeer()

CHIPort::returnCreditToPeer()
  if latency == 0:
    peer grantCredit()
  else:
    peer enqueueCreditGrant()

peer CHIPort::processCreditGrant()
  at grantTick:
    sample credit return latency
    grantCredit()
    sender credit++
```

### 11.2 endpoint receiver port，`cmn700_rtl`

```text
sender CHIPort::send()
  credit--
  connected endpoint CHIPort::receive()

endpoint CHIPort::receive()
  staging limit = rxbufNum
  rxbufOutstanding++
  push FIFO

endpoint OnHandleEventCallback_<CH>()
  receive_callback(head) succeeds
  pop FIFO
  releaseRxbufEntry(channel, curTick())
  delayed credit return
  sender credit++
```

endpoint 没有 MeshNode IB，因此 callback accept 就代表接收窗口可释放。

## 12. 统计项与诊断方式

### 12.1 CHIPort 统计

`CHIPortStats` 定义位于 [CHIPort.hh](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:145)。

| 统计项 | 代码位置 | 用途 |
|---|---|---|
| `credit_stall_events_by_channel` | [CHIPort.hh:149](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:149) | 发送端 credit 不足事件 |
| `credit_stall_cycles_by_channel` | [CHIPort.hh:150](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:150) | credit blocked 持续周期 |
| `no_credit_bubble_cycles_by_channel` | [CHIPort.hh:151](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:151) | sender 有发送需求但无 credit 的 bubble |
| `receive_callback_reject_events_by_channel` | [CHIPort.hh:152](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:152) | 下游 callback reject |
| `credit_return_latency_hist_*` | [CHIPort.hh:154-157](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:154) | release 到 grant 的 credit return latency |
| `rxbuf_occupancy_hist_*` | [CHIPort.hh:159-162](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:159) | 当前 staging FIFO occupancy，保留旧命名 |
| `skid_occupancy_hist_*` | [CHIPort.hh:164-167](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:164) | skid/staging FIFO occupancy |
| `rxbuf_outstanding_hist_*` | [CHIPort.hh:169-172](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:169) | combined RXBUF outstanding |
| `rxbuf_release_events_by_channel` | [CHIPort.hh:174](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:174) | combined RXBUF release 次数 |
| `deferred_credit_release_events_by_channel` | [CHIPort.hh:175](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/base/CHIPort.hh:175) | on_downstream_release 生效次数 |

诊断建议：

| 现象 | 优先查看 |
|---|---|
| credit bubble | `credit_stall_events_by_channel`, `no_credit_bubble_cycles_by_channel` |
| receiver window 没释放 | `rxbuf_outstanding_hist_*`, `rxbuf_release_events_by_channel` |
| flit 卡在 CHIPort staging | `skid_occupancy_hist_*`, `receive_callback_reject_events_by_channel` |
| delayed return latency 错 | `credit_return_latency_hist_*` |
| MeshNode downstream release 是否生效 | `deferred_credit_release_events_by_channel` |

### 12.2 MeshNode 统计

`MeshNodeStats` 定义位于 [MeshNode.hh](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.hh:101)。

| 统计项 | 代码位置 | 用途 |
|---|---|---|
| `voq_full_events` | [MeshNode.hh:117](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.hh:117) | legacy/current VOQ 或 RTL IB full 总事件 |
| `voq_backpressure_events_by_channel` | [MeshNode.hh:119](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.hh:119) | 按 channel 统计 backpressure |
| `ib_full_events_by_channel` | [MeshNode.hh:123](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.hh:123) | RTL IB full |
| `ib_occupancy_accum_by_channel` | [MeshNode.hh:124](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.hh:124) | IB occupancy 累计 |
| `ib_avg_occupancy_by_channel` | [MeshNode.hh:125](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.hh:125) | 平均 IB occupancy |
| `egress_credit_blocked_cycles_by_channel` | [MeshNode.hh:129](/nfs/home/wuchengkai/GEM5_reps/GEM5/src/mem/xsCHI/device/MeshNode.hh:129) | 下游 egress credit blocked |

诊断建议：

| 现象 | 优先查看 |
|---|---|
| flit 不能从 CHIPort staging 进入 MeshNode | `ib_full_events_by_channel`, `receive_callback_reject_events_by_channel` |
| flit 已进 IB 但不能送出 | `egress_credit_blocked_cycles_by_channel`, `egress_stall_cycles_by_dir` |
| ideal 实验被内部队列污染 | `voq_full_events`, `ib_full_events_by_channel` 应为 0 |

## 13. 与旧模式的行为差异

| 模式 | 接收窗口 | credit release 时机 | credit return latency |
|---|---|---|---|
| `legacy` | `recv_buffer_size/rxbufNum` 单一 FIFO 语义 | callback accept 后立即 release | 0 cycle |
| `cmn700` | `rxbuf_num` 作为单一接收 FIFO 窗口 | callback accept 后 release | Up/Dn/Internal delayed |
| `cmn700_rtl` endpoint port | `rxbuf_num` 作为 endpoint FIFO 窗口 | callback accept 后 release | Up/Dn/Internal delayed |
| `cmn700_rtl` MeshNode port | `rxbuf_num = skid_depth + ib_depth` combined window | egress send 成功、IB entry 释放后 release | Up/Dn/Internal delayed |

关键差异不是“是否 delayed credit return”，而是 credit return 的起点：

| 模式 | release 起点 |
|---|---|
| `cmn700` | flit 被 callback 接受，离开 CHIPort FIFO |
| `cmn700_rtl` MeshNode port | flit 离开 MeshNode IB/pending queue |

## 14. 当前实现边界与风险

| 边界/风险 | 说明 | 后续建议 |
|---|---|---|
| `outVoq` 仍同时承担 IB 与 per-egress pending queue | 当前通过 ingress/channel aggregate occupancy 近似 IB，但物理结构不是独立 IB array | 如需更贴 RTL，可拆分显式 IB slots 和 egress arbitration queue |
| C++ 未强制校验 `rxbuf == skid + ib` | 目前主要在 `CacheConfig.py` 校验，手写 SimObject 参数可能绕过 | 后续可在 MeshNode/CHIPort 连接阶段增加跨对象校验 |
| endpoint 不建 IB | 当前假设 endpoint callback accept 即可释放 receive window | 若 endpoint 也有内部 pipeline/queue，应引入 endpoint-side pending release |
| 不支持 `initial_credit_count > rxbuf_num` | 当前显式禁止 | 后续若建模 `DEV_XP_LL_CRD_COUNT_PARAM=4`，需要独立 safety window 模型 |
| `rxbuf_occupancy_hist_*` 命名可能误导 | 当前它采样的是 staging FIFO occupancy，不是 combined RXBUF | 建议后续文档/统计描述中优先看 `rxbuf_outstanding_hist_*` |
| `PortTransferLatency` 仍影响 staging callback 时机 | 它不是 CMN credit return latency | 对齐实验中不要用它替代 Up/Dn credit latency |
| `voq_depth` 仍保留 | legacy/current cmn700 使用；RTL 模式 `ib_depth=0` 时可能 fallback 到 `voq_depth` | 推荐 `cmn700_rtl` 实验显式设置 `--chi-ib-depth` |

## 15. 推荐运行配置

默认 CMN-700 RTL-like 配置：

```bash
--chi-credit-model=cmn700_rtl \
--chi-rxbuf-num=3 \
--chi-skid-depth=1 \
--chi-ib-depth=2 \
--chi-initial-credit-count=3 \
--chi-up-crd-lat-int=1 \
--chi-up-crd-lat-ext=2 \
--chi-dn-crd-lat-int=2 \
--chi-dn-crd-lat-ext=1 \
--chi-internal-crd-lat=1
```

用于对比的 current `cmn700` 配置：

```bash
--chi-credit-model=cmn700 \
--chi-rxbuf-num=3 \
--chi-up-crd-lat-int=1 \
--chi-up-crd-lat-ext=2 \
--chi-dn-crd-lat-int=2 \
--chi-dn-crd-lat-ext=1 \
--chi-internal-crd-lat=1
```

## 16. 最小验证矩阵

| 测试 | 配置 | 预期 |
|---|---|---|
| legacy 回归 | `--chi-credit-model=legacy` | callback accept 后立即返 credit，性能行为与旧模型一致 |
| cmn700 回归 | `--chi-credit-model=cmn700 --chi-rxbuf-num=3` | delayed credit return 生效，但 callback accept 即 release |
| RTL 默认窗口 | `cmn700_rtl, rxbuf=3, skid=1, ib=2` | 参数校验通过，initial credit=3 |
| 非法窗口 | `rxbuf=3, skid=2, ib=2` | 配置层报错 |
| IB full | 下游 blocked，持续注入 | flit 留在 staging，`ib_full_events_by_channel` 与 callback reject 上升 |
| deferred release | flit 进入 IB 后 egress blocked | `rxbufOutstanding` 不下降，sender credit 不增加 |
| egress release | egress send 成功 | `rxbuf_release_events_by_channel` 增加，之后 delayed grant |
| credit latency | Up/Dn/Internal 各方向 | `credit_return_latency_hist_*` 等于配置 latency |

## 17. 已执行过的基础验证

以下验证用于确认当前代码可编译、基础 MeshNode 测试可运行：

| 验证 | 结果 |
|---|---|
| `python3 -m py_compile configs/common/Options.py configs/common/CacheConfig.py configs/common/xiangshan.py` | 通过 |
| `git diff --check` 针对相关修改文件 | 通过 |
| `scons -j8 build/RISCV/mem/xsCHI/test/testMeshNodeIntegration.test.opt build/RISCV/mem/xsCHI/test/testMeshNode.test.opt` | 通过 |
| `build/RISCV/mem/xsCHI/test/testMeshNode.test.opt` | 5/5 通过 |
| `build/RISCV/mem/xsCHI/test/testMeshNodeIntegration.test.opt` | 6/6 通过 |

已知未完成项：

| 项 | 状态 |
|---|---|
| `testRnHnDramPathIntegration.test.opt` | 构建阶段被已有缺失头文件 `mem/xsCHI/device/HNF.hh` 阻塞，未能验证完整 RN/HN/DRAM 路径 |

## 18. 后续改进建议

| 优先级 | 建议 | 原因 |
|---|---|---|
| P0 | 增加专门覆盖 `cmn700_rtl` 的单元测试 | 当前基础 MeshNode 测试通过，但还需要专门证明 deferred release 与 IB full 行为 |
| P0 | 在 stats 分析脚本中区分 skid occupancy、rxbuf outstanding、IB occupancy | 避免把 staging FIFO 当作 combined RXBUF |
| P1 | 在 C++ 侧增加跨对象参数一致性检查 | 防止绕过 Python 配置层创建非法 `rxbuf/skid/ib` 组合 |
| P1 | 完整访存路径回归：SLC hit、peer hit、DDR path | 确认 credit 机制不破坏 CHI transaction lifecycle |
| P2 | 显式拆分 IB slots 与 post-IB arbitration queue | 更贴近 CMN RTL，但会扩大改动面 |
| P2 | 支持 LL credit count 大于 RXBUF 的安全模型 | 为后续建模 `DEV_XP_LL_CRD_COUNT_PARAM=4` 留接口 |
