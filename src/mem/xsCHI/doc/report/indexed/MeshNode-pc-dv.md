# MeshNode 性能计数器开发与诊断记录（MeshNode-pc-dv）

> 更新时间：2026-03-24  
> 作用：沉淀 MeshNode 计数器开发的“参考→规划→实施”全过程，并提供 Phase1/2/3 完整计数器清单与 stats.txt 读数解读模板，供后续仿真调优与回归使用。  
> 说明：当前 Phase3 已落地范围为 **P2-Min**（协议计数 + `WaitCompAck`）；`hnf_in_trans_latency_hist` 仍为后续补全项（`[待补充]`）。

## 0. 2026-03-24 重构增补（P0 + P1 兼容）


| 变更点                 | 落地说明                                                                                                                 |
| ------------------- | -------------------------------------------------------------------------------------------------------------------- |
| `per_ingress` 口径显式化 | 新增 `voq_depth_mode_per_ingress`（`1=per_ingress, 0=aggregate`）作为运行口径标签。                                               |
| VOQ 可观测性增强          | 新增 `voq_depth_accum/avg_by_egress_channel` 与 `voq_depth_accum/avg_by_egress_channel_ingress`，用于定位热点 channel/ingress。 |
| wall-clock 归一化增强    | 新增 `wall_cycles`、`dir_link_util_wall`、`voq_depth_accum/avg_by_egress_wall`。保留旧 `send_event` 口径字段兼容脚本。                |
| 阻塞原因细分              | 新增 `egress_no_credit_cycles_by_dir`、`egress_no_send_other_cycles_by_dir`、`egress_residual_backlog_cycles_by_dir`。    |
| hop/e2e 边界修正        | 在 CHIBridge/FakeL3/CHI_L3/DDRWrapper 对新注入 flit 清理 mesh 元数据，避免跨段统计串接。                                                 |
| 协议别名方向化             | CHIBridge/FakeL3 新增 `protocol_*_{tx,rx}_total`，旧 `protocol_*_total` 继续保留。                                            |


---

# 任务1：整理添加计数器的完整流程

## 1. 参考阶段：Ruby stats 提炼方法与结论

### 1.1 输入与方法


| 项    | 内容                                         |
| ---- | ------------------------------------------ |
| 参考对象 | Ruby 缓存子系统 `stats.txt`（重点 `system.ruby.`*） |
| 分析方法 | 按“流量→延迟→争用→协议语义”分层归类，筛选可迁移指标               |
| 分析目标 | 为 classic cache + CHI + MeshNode 建立可观测指标基线 |


### 1.2 提炼出的分类维度


| 维度                      | 关注内容                         | 对 MeshNode 的价值   |
| ----------------------- | ---------------------------- | ---------------- |
| 网络流量（Traffic）           | 消息数、flit 数、按方向流量             | 识别热点链路与负载分布      |
| 网络延迟（Latency）           | 请求/响应延迟、端到端时延                | 判断慢点是否来自网络路径     |
| 争用与背压（Contention）       | 队列满、重试、阻塞周期                  | 定位瓶颈和拥塞根因        |
| 协议事务（Protocol）          | Read/Write/CompAck/Snp 等事务行为 | 将“网络现象”映射到“协议语义” |
| 一致性活动（Coherence）        | snoop/probe/invalidation 相关  | 判断一致性流量贡献        |
| HN/目录处理（Home/Directory） | HN 事务处理和状态变化                 | 区分“网络慢”与“HN处理慢”  |


### 1.3 参考阶段结论


| 结论                               | 影响                           |
| -------------------------------- | ---------------------------- |
| 仅有总量计数不足以解释性能下降                  | 必须补充拥塞与时延分布类指标               |
| 仅有网络指标无法回答“哪类事务导致慢”              | 必须补充协议 opcode 级计数与关键事务等待时间   |
| Ruby 指标可做目标画像，但与 classic 架构不完全同构 | 采用“复用 + 适配 + Mesh特有新增”三类映射策略 |


---

## 2. 规划阶段：Ruby 指标到 MeshNode 的映射与分期

### 2.1 映射策略


| 类型        | 判定标准             | 示例                                                |
| --------- | ---------------- | ------------------------------------------------- |
| 直接复用      | 统计语义与采样点一致       | 消息总量、按 channel flits、方向利用率、VOQ full 事件            |
| 架构适配      | 语义相近但实现路径不同      | `stall/bw_sat` 需基于 `MeshNode::onSendEvent` 调度语义定义 |
| Mesh 特有新增 | Ruby 不直接提供或粒度不匹配 | 方向拥塞、hop 分布、e2e 延迟分布                              |


### 2.2 为什么分 Phase1/2/3


| 阶段         | 划分依据             | 核心目标        |
| ---------- | ---------------- | ----------- |
| Phase1（P0） | 依赖少、实现快、先建基础可见性  | 回答“哪里忙、哪里堵” |
| Phase2（P1） | 在 P0 基础上补拥塞与时延因果 | 回答“为什么慢”    |
| Phase3（P2） | 引入协议事务语义，复杂度中等   | 回答“哪类事务导致慢” |


---

## 3. 实施阶段：Phase1/2/3 结果

### 3.1 Phase1 实施内容与效果


| 项    | 内容                                                                                                                               |
| ---- | -------------------------------------------------------------------------------------------------------------------------------- |
| 实施内容 | `msg_count_control/data`、`msg_byte_data`、`ingress/egress_flits_by_channel`、`dir_egress_flits`、`dir_link_util`、`voq_full_events`* |
| 解决问题 | 建立基础流量热度图，识别热点方向与回压口                                                                                                             |


### 3.2 Phase2 实施内容与效果


| 项    | 内容                                                                                                                                 |
| ---- | ---------------------------------------------------------------------------------------------------------------------------------- |
| 实施内容 | `voq_depth_accum/avg_by_egress`、`egress_stall_cycles_by_dir`、`egress_bw_sat_cycles_by_dir`、`hop_count_hist_*`、`e2e_latency_hist_*` |
| 新增能力 | 能区分“队列堆积”“发送受阻”“路径拉长”三类慢因                                                                                                          |


### 3.3 Phase3（当前 P2-Min）实施内容与效果


| 项    | 内容                                                                                                                                                                                                    |
| ---- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 实施内容 | `protocol_tx_by_opcode`、`protocol_rx_by_opcode`、`protocol_readshared_total`、`protocol_writeevict_total`、`protocol_compack_total`、`protocol_snp_total`（CHIBridge + FakeL3）；`wait_compack_*`（CHIBridge） |
| 最终效果 | 将网络拥塞映射到具体协议事务；可量化 CompAck 等待是否成为闭环瓶颈                                                                                                                                                                 |


### 3.4 Phase3 后续补全项


| 项                                      | 状态      |
| -------------------------------------- | ------- |
| `hnf_in_trans_latency_hist`（HN 内部事务时延） | `[待补充]` |


---

# 任务2：输出 Phase1/2/3 计数器完整清单

## 1. Phase1 计数器


| 阶段     | 计数器名称                        | 含义                        | 所在文件/位置                                                        | 所属类别  |
| ------ | ---------------------------- | ------------------------- | -------------------------------------------------------------- | ----- |
| Phase1 | `msg_count_control`          | 控制类 flit 发送总数             | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 网络流量  |
| Phase1 | `msg_count_data`             | 数据类 flit 发送总数             | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 网络流量  |
| Phase1 | `msg_byte_data`              | 数据通道总字节量                  | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 网络流量  |
| Phase1 | `ingress_flits_by_channel`   | 按 CHI 通道统计入站 flit 数       | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 网络流量  |
| Phase1 | `egress_flits_by_channel`    | 按 CHI 通道统计出站 flit 数       | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 网络流量  |
| Phase1 | `dir_egress_flits`           | 按方向统计出站 flit 数            | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 网络流量  |
| Phase1 | `local_egress_flits`         | 按本地端口统计出站 flit 数         | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 网络流量  |
| Phase1 | `dir_active_cycles`          | 按方向统计活跃发送周期               | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 链路利用  |
| Phase1 | `local_active_cycles`        | 按本地端口统计活跃发送周期           | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 链路利用  |
| Phase1 | `send_event_cycles`          | 调度器处理总周期                  | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 基线归一化 |
| Phase1 | `wall_cycles`                | wall-clock 观测周期（按调度时间戳推导） | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 基线归一化 |
| Phase1 | `dir_link_util`              | 方向链路利用率（公式）               | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 链路利用  |
| Phase1 | `dir_link_util_wall`         | 方向链路利用率（wall-cycle 公式）    | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 链路利用  |
| Phase1 | `local_link_util`            | 本地端口利用率（公式）               | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 链路利用  |
| Phase1 | `local_link_util_wall`       | 本地端口利用率（wall-cycle 公式）   | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 链路利用  |
| Phase1 | `voq_depth_mode_per_ingress` | VOQ 判满模式标签（1/0）           | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 口径标签  |
| Phase1 | `voq_full_events`            | VOQ 满导致回压总次数              | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 争用与背压 |
| Phase1 | `voq_full_events_by_egress`  | 按 egress 统计 VOQ 满事件       | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 争用与背压 |

### Phase1 注释：`wall_cycles` 口径

| 项 | 说明 |
| --- | --- |
| 定义 | `wall_cycles` 表示调度器两次执行之间流逝的“真实周期”累计值。实现上在每次 `onSendEvent` 内计算 `cycleDelta=(nowTick-lastSendEventTick)/clockPeriod`（首个事件记 1），再累加到 `wall_cycles`。 |
| 与 `send_event_cycles` 区别 | `send_event_cycles` 是“调度器执行次数”；`wall_cycles` 是“调度器观测到的时间流逝”。事件稀疏时，`wall_cycles` 会显著大于 `send_event_cycles`。 |
| 典型用途 | 用于补齐事件驱动采样分母偏差，例如 `dir_link_util_wall` / `local_link_util_wall` 与 `voq_avg_depth_by_egress_wall`。 |
| 解读注意 | `wall_cycles` 仍是基于调度时间戳的离散观测，不等同于全系统 `simCycles`；但跨实验可比性通常优于纯 `send_event_cycles` 口径。 |


## 2. Phase2 计数器


| 阶段     | 计数器名称                                       | 含义                                       | 所在文件/位置                                                        | 所属类别  |
| ------ | ------------------------------------------- | ---------------------------------------- | -------------------------------------------------------------- | ----- |
| Phase2 | `voq_depth_accum_by_egress`                 | 每周期采样并累加 egress 队列深度                     | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 队列与拥塞 |
| Phase2 | `voq_depth_accum_by_egress_wall`            | 按 wall-cycle 加权累加 egress 队列深度            | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 队列与拥塞 |
| Phase2 | `voq_avg_depth_by_egress`                   | egress 平均队列深度（公式）                        | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 队列与拥塞 |
| Phase2 | `voq_avg_depth_by_egress_wall`              | egress 平均队列深度（wall-cycle 公式）             | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 队列与拥塞 |
| Phase2 | `voq_depth_accum_by_egress_channel`         | 每周期采样并累加 `(egress,channel)` 队列深度         | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 队列与拥塞 |
| Phase2 | `voq_avg_depth_by_egress_channel`           | `(egress,channel)` 平均队列深度（公式）            | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 队列与拥塞 |
| Phase2 | `voq_depth_accum_by_egress_channel_ingress` | 每周期采样并累加 `(egress,channel,ingress)` 队列深度 | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 队列与拥塞 |
| Phase2 | `voq_avg_depth_by_egress_channel_ingress`   | `(egress,channel,ingress)` 平均队列深度（公式）    | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 队列与拥塞 |
| Phase2 | `egress_stall_cycles_by_dir`                | 有待发但本周期未发出的方向周期数                         | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 争用与阻塞 |
| Phase2 | `egress_bw_sat_cycles_by_dir`               | 发出后仍积压的方向周期数                             | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 带宽饱和  |
| Phase2 | `egress_no_credit_cycles_by_dir`            | 有待发且未发出，且 credit 阻塞的方向周期数                | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 争用与阻塞 |
| Phase2 | `egress_no_send_other_cycles_by_dir`        | 有待发且未发出，非 credit 原因的方向周期数                | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 争用与阻塞 |
| Phase2 | `egress_residual_backlog_cycles_by_dir`     | 已发出但仍有积压的方向周期数（语义别名）                     | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 带宽饱和  |
| Phase2 | `hop_count_hist_snp`                        | SNP 通道 hop 分布                            | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 路径与延迟 |
| Phase2 | `hop_count_hist_req`                        | REQ 通道 hop 分布                            | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 路径与延迟 |
| Phase2 | `hop_count_hist_rsp`                        | RSP 通道 hop 分布                            | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 路径与延迟 |
| Phase2 | `hop_count_hist_dat`                        | DAT 通道 hop 分布                            | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 路径与延迟 |
| Phase2 | `e2e_latency_hist_snp`                      | SNP 通道端到端时延分布                            | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 网络延迟  |
| Phase2 | `e2e_latency_hist_req`                      | REQ 通道端到端时延分布                            | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 网络延迟  |
| Phase2 | `e2e_latency_hist_rsp`                      | RSP 通道端到端时延分布                            | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 网络延迟  |
| Phase2 | `e2e_latency_hist_dat`                      | DAT 通道端到端时延分布                            | `src/mem/xsCHI/device/MeshNode.cc` / `MeshNode::MeshNodeStats` | 网络延迟  |

### Phase2 注释：`hop_count_hist_*` 与 `e2e_latency_hist_*`

| 项 | 说明 |
| --- | --- |
| `hop_count_hist_*` 统计对象 | flit 级（按通道分 SNP/REQ/RSP/DAT），不是 transaction 级。 |
| hop 起点 | flit 从 **local ingress** 首次进入 mesh 且 `mesh_stats_valid=false` 时，初始化 `inject_tick=curTick`、`hop=0`。 |
| hop 计数规则 | 每次成功跨方向口（E/W/N/S）发送前 `hop+1`；发送失败会回滚，避免重试导致重复计 hop。 |
| hop 采样点 | flit 从 mesh **local egress 成功发送**时采样，写入 `hop_count_hist_*`。 |
| `e2e_latency_hist_*` 定义 | 在同一 local 交付采样点记录 `curTick - injectTick`，单位是 **Tick**。 |
| Tick 与 cycle 换算 | `latency_cycle = latency_tick / clockPeriod`。报告中若展示 cycle，需显式使用该换算。 |
| 时间窗口边界 | 单个 flit 从“首次本地注入 mesh”到“本地端口成功交付”，包含排队、仲裁、链路/端口传输等网络内等待。 |
| 边界一致性前提 | CHIBridge/FakeL3/CHI_L3/DDRWrapper 对重新注入 mesh 的 flit 会清理 mesh 元数据，避免跨段 hop/e2e 串接。 |


## 3. Phase3（当前 P2-Min）计数器

### 3.1 CHIBridge


| 阶段     | 计数器名称                          | 含义                         | 所在文件/位置                                                        | 所属类别  |
| ------ | ------------------------------ | -------------------------- | -------------------------------------------------------------- | ----- |
| Phase3 | `protocol_tx_by_opcode`        | RN 侧按 opcode 统计发送成功 flit 数 | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议事务  |
| Phase3 | `protocol_rx_by_opcode`        | RN 侧按 opcode 统计接收 flit 数   | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议事务  |
| Phase3 | `protocol_readshared_total`    | `READSHARED` 总观测数          | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议事务  |
| Phase3 | `protocol_readshared_tx_total` | `READSHARED` TX 观测数        | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议事务  |
| Phase3 | `protocol_readshared_rx_total` | `READSHARED` RX 观测数        | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议事务  |
| Phase3 | `protocol_writeevict_total`    | `WRITEEVICT*` 总观测数         | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议事务  |
| Phase3 | `protocol_writeevict_tx_total` | `WRITEEVICT*` TX 观测数       | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议事务  |
| Phase3 | `protocol_writeevict_rx_total` | `WRITEEVICT*` RX 观测数       | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议事务  |
| Phase3 | `protocol_compack_total`       | `COMPACK` 总观测数             | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议事务  |
| Phase3 | `protocol_compack_tx_total`    | `COMPACK` TX 观测数           | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议事务  |
| Phase3 | `protocol_compack_rx_total`    | `COMPACK` RX 观测数           | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议事务  |
| Phase3 | `protocol_snp_total`           | snoop opcode 总观测数          | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 一致性活动 |
| Phase3 | `protocol_snp_tx_total`        | snoop opcode TX 观测数        | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 一致性活动 |
| Phase3 | `protocol_snp_rx_total`        | snoop opcode RX 观测数        | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 一致性活动 |
| Phase3 | `wait_compack_cycles`          | COMPACK 累计等待周期             | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议时延  |
| Phase3 | `wait_compack_cycles_hist`     | COMPACK 等待周期分布             | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议时延  |
| Phase3 | `wait_compack_sent_total`      | 成功发送 COMPACK 总数            | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议事务  |
| Phase3 | `wait_compack_avg_cycles`      | 平均每个 COMPACK 等待周期（公式）      | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 协议时延  |
| Phase3 | `wait_compack_pending_max`     | COMPACK 队列峰值深度             | `src/mem/xsCHI/device/CHIBridge.cc` / `CHIBridge::BridgeStats` | 争用与背压 |


### 3.2 FakeL3（HNF）


| 阶段     | 计数器名称                          | 含义                         | 所在文件/位置                                                  | 所属类别  |
| ------ | ------------------------------ | -------------------------- | -------------------------------------------------------- | ----- |
| Phase3 | `protocol_tx_by_opcode`        | HN 侧按 opcode 统计发送成功 flit 数 | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 协议事务  |
| Phase3 | `protocol_rx_by_opcode`        | HN 侧按 opcode 统计接收 flit 数   | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 协议事务  |
| Phase3 | `protocol_readshared_total`    | HN 侧 `READSHARED` 总观测数     | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 协议事务  |
| Phase3 | `protocol_readshared_tx_total` | HN 侧 `READSHARED` TX 观测数   | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 协议事务  |
| Phase3 | `protocol_readshared_rx_total` | HN 侧 `READSHARED` RX 观测数   | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 协议事务  |
| Phase3 | `protocol_writeevict_total`    | HN 侧 `WRITEEVICT*` 总观测数    | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 协议事务  |
| Phase3 | `protocol_writeevict_tx_total` | HN 侧 `WRITEEVICT*` TX 观测数  | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 协议事务  |
| Phase3 | `protocol_writeevict_rx_total` | HN 侧 `WRITEEVICT*` RX 观测数  | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 协议事务  |
| Phase3 | `protocol_compack_total`       | HN 侧 `COMPACK` 总观测数        | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 协议事务  |
| Phase3 | `protocol_compack_tx_total`    | HN 侧 `COMPACK` TX 观测数      | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 协议事务  |
| Phase3 | `protocol_compack_rx_total`    | HN 侧 `COMPACK` RX 观测数      | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 协议事务  |
| Phase3 | `protocol_snp_total`           | HN 侧 snoop opcode 总观测数     | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 一致性活动 |
| Phase3 | `protocol_snp_tx_total`        | HN 侧 snoop opcode TX 观测数   | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 一致性活动 |
| Phase3 | `protocol_snp_rx_total`        | HN 侧 snoop opcode RX 观测数   | `src/mem/xsCHI/device/fakeL3.cc` / `FakeL3::FakeL3Stats` | 一致性活动 |


## 4. 关键名词说明表


| 术语                    | 定义                                          |
| --------------------- | ------------------------------------------- |
| flit                  | NoC 的最小流控传输单元（flow control digit）。          |
| packet                | 由多个 flit 组成的逻辑报文。                           |
| REQ/RSP/DAT/SNP       | CHI 四条通道：请求/响应/数据/snoop。                    |
| snoop                 | 一致性探测消息，用于查询或更新其他缓存副本状态。                    |
| probe                 | 一致性协议中的探测动作，常作为 snoop 同义语境使用。               |
| backpressure          | 下游拥塞时向上游传播“暂停发送”的反压机制。                      |
| VOQ                   | Virtual Output Queue，按输出口分离的虚拟队列，降低 HOL 阻塞。 |
| HOL blocking          | 队头阻塞，队首消息被阻塞导致后续消息无法前进。                     |
| RR 仲裁                 | Round-Robin 轮询仲裁，保障多输入竞争下公平性。               |
| CompAck/COMPACK       | CHI 完成确认响应，用于事务收敛闭环。                        |
| DBIDRESP/COMPDBIDRESP | 写回路径中的 DBID 分配与确认响应事务。                      |
| RN/HN/SN              | Request Node / Home Node / Slave Node。      |
| e2e latency           | 消息从注入到目的端送达的端到端时延。                          |


---

# 任务3：生成 stats.txt 字段清单与读数解读模板

## 1. 预期字段清单（按类别）

> 字段路径说明：  
> `\<mesh_path\>` / `\<bridge_path\>` / `\<hn_path\>` 需替换为你的真实对象层级路径（`[待补充]`）。  
> 例如 `system....MeshNode0`、`system....CHIBridge`、`system....FakeL3`（`[待补充]`）。

### 1.1 网络流量与利用（MeshNode，`network` 组）


| 预期字段                                                                                     | 数据类型     | 类别    |
| ---------------------------------------------------------------------------------------- | -------- | ----- |
| `\<mesh_path\>.network.msg_count_control`                                                | 累计值      | 网络流量  |
| `\<mesh_path\>.network.msg_count_data`                                                   | 累计值      | 网络流量  |
| `\<mesh_path\>.network.msg_byte_data`                                                    | 累计值      | 网络流量  |
| `\<mesh_path\>.network.ingress_flits_by_channel::{SNP,REQ,RSP,DAT}`                      | 累计值（向量）  | 网络流量  |
| `\<mesh_path\>.network.egress_flits_by_channel::{SNP,REQ,RSP,DAT}`                       | 累计值（向量）  | 网络流量  |
| `\<mesh_path\>.network.dir_egress_flits::{east,west,north,south}`                        | 累计值（向量）  | 网络流量  |
| `\<mesh_path\>.network.local_egress_flits::{local0,local1}`                              | 累计值（向量）  | 网络流量  |
| `\<mesh_path\>.network.dir_active_cycles::{east,west,north,south}`                       | 累计值（向量）  | 链路利用  |
| `\<mesh_path\>.network.local_active_cycles::{local0,local1}`                             | 累计值（向量）  | 链路利用  |
| `\<mesh_path\>.network.send_event_cycles`                                                | 累计值      | 基线归一化 |
| `\<mesh_path\>.network.wall_cycles`                                                      | 累计值      | 基线归一化 |
| `\<mesh_path\>.network.dir_link_util::{east,west,north,south}`                           | 百分比（公式）  | 链路利用  |
| `\<mesh_path\>.network.dir_link_util_wall::{east,west,north,south}`                      | 百分比（公式）  | 链路利用  |
| `\<mesh_path\>.network.local_link_util::{local0,local1}`                                 | 百分比（公式）  | 链路利用  |
| `\<mesh_path\>.network.local_link_util_wall::{local0,local1}`                            | 百分比（公式）  | 链路利用  |
| `\<mesh_path\>.network.voq_depth_mode_per_ingress`                                       | 标记值（0/1） | 口径标签  |
| `\<mesh_path\>.network.voq_full_events`                                                  | 累计值      | 争用与背压 |
| `\<mesh_path\>.network.voq_full_events_by_egress::{local0,local1,east,west,north,south}` | 累计值（向量）  | 争用与背压 |


### 1.2 拥塞与时延（MeshNode，`network` 组）


| 预期字段                                                                                          | 数据类型    | 类别    |
| --------------------------------------------------------------------------------------------- | ------- | ----- |
| `\<mesh_path\>.network.voq_depth_accum_by_egress::{local0,local1,east,west,north,south}`      | 累计值（向量） | 队列与拥塞 |
| `\<mesh_path\>.network.voq_depth_accum_by_egress_wall::{local0,local1,east,west,north,south}` | 累计值（向量） | 队列与拥塞 |
| `\<mesh_path\>.network.voq_avg_depth_by_egress::{local0,local1,east,west,north,south}`        | 平均值（公式） | 队列与拥塞 |
| `\<mesh_path\>.network.voq_avg_depth_by_egress_wall::{local0,local1,east,west,north,south}`   | 平均值（公式） | 队列与拥塞 |
| `\<mesh_path\>.network.voq_depth_accum_by_egress_channel::<egress.channel>`                   | 累计值（向量） | 队列与拥塞 |
| `\<mesh_path\>.network.voq_avg_depth_by_egress_channel::<egress.channel>`                     | 平均值（公式） | 队列与拥塞 |
| `\<mesh_path\>.network.voq_depth_accum_by_egress_channel_ingress::<egress.channel.ingress>`   | 累计值（向量） | 队列与拥塞 |
| `\<mesh_path\>.network.voq_avg_depth_by_egress_channel_ingress::<egress.channel.ingress>`     | 平均值（公式） | 队列与拥塞 |
| `\<mesh_path\>.network.egress_stall_cycles_by_dir::{east,west,north,south}`                   | 累计值（向量） | 争用与阻塞 |
| `\<mesh_path\>.network.egress_bw_sat_cycles_by_dir::{east,west,north,south}`                  | 累计值（向量） | 带宽饱和  |
| `\<mesh_path\>.network.egress_no_credit_cycles_by_dir::{east,west,north,south}`               | 累计值（向量） | 争用与阻塞 |
| `\<mesh_path\>.network.egress_no_send_other_cycles_by_dir::{east,west,north,south}`           | 累计值（向量） | 争用与阻塞 |
| `\<mesh_path\>.network.egress_residual_backlog_cycles_by_dir::{east,west,north,south}`        | 累计值（向量） | 带宽饱和  |
| `\<mesh_path\>.network.hop_count_hist_{snp,req,rsp,dat}`                                      | 分布      | 路径与延迟 |
| `\<mesh_path\>.network.e2e_latency_hist_{snp,req,rsp,dat}`                                    | 分布      | 网络延迟  |


### 1.3 协议语义（CHIBridge，`protocol` 组）


| 预期字段                                                            | 数据类型     | 类别    |
| --------------------------------------------------------------- | -------- | ----- |
| `\<bridge_path\>.protocol.protocol_tx_by_opcode::<CHI_OP_TYPE>` | 累计值（向量）  | 协议事务  |
| `\<bridge_path\>.protocol.protocol_rx_by_opcode::<CHI_OP_TYPE>` | 累计值（向量）  | 协议事务  |
| `\<bridge_path\>.protocol.protocol_readshared_total`            | 累计值      | 协议事务  |
| `\<bridge_path\>.protocol.protocol_readshared_tx_total`         | 累计值      | 协议事务  |
| `\<bridge_path\>.protocol.protocol_readshared_rx_total`         | 累计值      | 协议事务  |
| `\<bridge_path\>.protocol.protocol_writeevict_total`            | 累计值      | 协议事务  |
| `\<bridge_path\>.protocol.protocol_writeevict_tx_total`         | 累计值      | 协议事务  |
| `\<bridge_path\>.protocol.protocol_writeevict_rx_total`         | 累计值      | 协议事务  |
| `\<bridge_path\>.protocol.protocol_compack_total`               | 累计值      | 协议事务  |
| `\<bridge_path\>.protocol.protocol_compack_tx_total`            | 累计值      | 协议事务  |
| `\<bridge_path\>.protocol.protocol_compack_rx_total`            | 累计值      | 协议事务  |
| `\<bridge_path\>.protocol.protocol_snp_total`                   | 累计值      | 一致性活动 |
| `\<bridge_path\>.protocol.protocol_snp_tx_total`                | 累计值      | 一致性活动 |
| `\<bridge_path\>.protocol.protocol_snp_rx_total`                | 累计值      | 一致性活动 |
| `\<bridge_path\>.protocol.wait_compack_cycles`                  | 累计值      | 协议时延  |
| `\<bridge_path\>.protocol.wait_compack_cycles_hist`             | 分布       | 协议时延  |
| `\<bridge_path\>.protocol.wait_compack_sent_total`              | 累计值      | 协议事务  |
| `\<bridge_path\>.protocol.wait_compack_avg_cycles`              | 平均值（公式）  | 协议时延  |
| `\<bridge_path\>.protocol.wait_compack_pending_max`             | 峰值（累计输出） | 争用与背压 |


### 1.4 协议语义（FakeL3，`protocol` 组）


| 预期字段                                                        | 数据类型    | 类别    |
| ----------------------------------------------------------- | ------- | ----- |
| `\<hn_path\>.protocol.protocol_tx_by_opcode::<CHI_OP_TYPE>` | 累计值（向量） | 协议事务  |
| `\<hn_path\>.protocol.protocol_rx_by_opcode::<CHI_OP_TYPE>` | 累计值（向量） | 协议事务  |
| `\<hn_path\>.protocol.protocol_readshared_total`            | 累计值     | 协议事务  |
| `\<hn_path\>.protocol.protocol_readshared_tx_total`         | 累计值     | 协议事务  |
| `\<hn_path\>.protocol.protocol_readshared_rx_total`         | 累计值     | 协议事务  |
| `\<hn_path\>.protocol.protocol_writeevict_total`            | 累计值     | 协议事务  |
| `\<hn_path\>.protocol.protocol_writeevict_tx_total`         | 累计值     | 协议事务  |
| `\<hn_path\>.protocol.protocol_writeevict_rx_total`         | 累计值     | 协议事务  |
| `\<hn_path\>.protocol.protocol_compack_total`               | 累计值     | 协议事务  |
| `\<hn_path\>.protocol.protocol_compack_tx_total`            | 累计值     | 协议事务  |
| `\<hn_path\>.protocol.protocol_compack_rx_total`            | 累计值     | 协议事务  |
| `\<hn_path\>.protocol.protocol_snp_total`                   | 累计值     | 一致性活动 |
| `\<hn_path\>.protocol.protocol_snp_tx_total`                | 累计值     | 一致性活动 |
| `\<hn_path\>.protocol.protocol_snp_rx_total`                | 累计值     | 一致性活动 |


---

## 2. 读数解读模板（正常范围/异常信号/调优方向）

> 说明：阈值是工程起步参考，建议基于你的 baseline workload 做二次标定（`[待补充]`）。


| 指标（类）                                                                                          | 正常范围参考                                                                 | 异常信号                                                                                                      | 调优方向                                                                                     |
| ---------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------- |
| `wall_cycles / send_event_cycles`                                                              | 持续负载下通常接近 1~3；突发/空闲阶段可明显增大                                             | 比值长期偏大（如 >5）但仍仅用 scheduler 口径判断拥塞，容易误判                                                                   | 优先切到 `*_wall` 指标（`dir_link_util_wall`、`local_link_util_wall`、`voq_avg_depth_by_egress_wall`） |
| `dir_link_util` + `dir_link_util_wall`                                                         | 两者趋势应一致；wall 口径通常更保守                                                    | `dir_link_util` 高而 `dir_link_util_wall` 低，说明事件稀疏导致 scheduler 口径偏高                                        | 报告主口径改用 `dir_link_util_wall`，scheduler 口径仅做相对变化参考                                        |
| `local_link_util` + `local_link_util_wall`                                                     | `local0/local1` 应与业务映射预期一致                                                | 单本地口长期高位（如 >85%）且另一口明显空闲，或 wall/scheduler 差异过大                                                           | 复查 endpoint 映射与本地收包倾斜，必要时调整任务绑定或地址映射                                                    |
| `local_egress_flits`                                                                            | 与预期接收方向一致；双端口负载通常不应长期极端失衡                                               | `local_egress_flits::local0` 与 `::local1` 长期严重失衡（如 >3:1）                                                   | 结合 `local_link_util_wall` 排查是否存在单口热点与上游流量偏置                                              |
| `voq_full_events` + `voq_depth_mode_per_ingress`                                               | 低负载接近 0；同类 workload 跨实验比较时 mode 应一致                                      | `voq_full_events` 快速增长且 mode 与对照实验不同（`1=per_ingress,0=aggregate`）                                       | 先统一 `voq_depth_mode`，再比较绝对值；必要时提高 `voq_depth` 做 A/B                                      |
| `voq_avg_depth_by_egress` + `voq_avg_depth_by_egress_wall`                                     | 常见低于 `voq_depth` 的 30%~40%                                                 | scheduler 口径与 wall 口径背离；或 wall 口径持续 >60%                                                                  | 以 wall 口径作为主判断，并联查对应 egress 的 credit 返回与消费能力                                             |
| `voq_avg_depth_by_egress_channel` / `voq_avg_depth_by_egress_channel_ingress`                 | 热点应集中在少数 `(egress,channel)` 或 `(egress,channel,ingress)`                | 总体 `voq_avg_depth_by_egress` 不高，但细粒度某一 ingress/channel 长期高位                                                 | 使用细粒度计数器定位“谁在挤占队列”，再针对性优化流量入口或通道分配                                                      |
| `egress_no_credit_cycles_by_dir` + `egress_no_send_other_cycles_by_dir`                       | 两者占比随负载上升但不应长期单边压倒                                                       | `no_credit` 持续高：下游 credit 回流慢；`no_send_other` 持续高：仲裁/端口可用性问题                                              | 按根因拆分处理：credit 路径优先看下游消费；other 路径优先看仲裁策略与端口竞争                                           |
| `egress_residual_backlog_cycles_by_dir`（与 `egress_bw_sat_cycles_by_dir` 等价语义）              | 中高负载可出现，通常随热点变化短时波动                                                      | 长期高位（如 >5%~10%）且与 `dir_link_util_wall` 同向抬升                                                               | 评估链路是否接近带宽上限，优化流量映射或降低热点注入                                                            |
| `hop_count_hist_*`                                                                              | 主峰应贴近拓扑曼哈顿距离                                                             | 主峰右移或长尾明显拉长                                                                                               | 复查源-目的映射与跨节点通信路径；确认是否出现异常远距通信                                                          |
| `e2e_latency_hist_*`                                                                            | p99/p50 一般 <2~3（以 cycle 口径报告时需先由 Tick 换算）                                | p99 突增且长尾变厚                                                                                               | 联动排查 `voq_*`、`egress_*` 与 `hop_count_hist_*`，区分“排队慢”与“路径变长”                               |
| `protocol_*_by_opcode` + `protocol_*_{tx,rx}_total`                                            | 与 workload 协议行为匹配                                                         | 某 opcode 或某方向（tx/rx）异常突增                                                                                   | 反查上游请求类型与协议分支，确认是否由业务变化或错误重试导致                                                         |
| `wait_compack_avg_cycles` + `wait_compack_pending_max`                                         | 低负载常接近 0~2 cycles                                                         | `wait_compack_avg_cycles > 5` 且 `pending_max` 抬升                                                           | 优先检查 ACK 回程路径与 RSP 方向拥塞                                                                     |
| `protocol_snp_total`                                                                            | 当前版本一般接近 0                                                                | 明显非 0                                                                                                      | snoop 主路径仍在演进，先核查触发源、配置与一致性分支覆盖度                                                         |


---

## 3. 快速诊断 Checklist（8 项，含新计数器）

1. 先看 `wall_cycles / send_event_cycles`：如果比值长期偏大（如 >5），优先采用 `*_wall` 口径解读，避免 scheduler 分母导致高估。
2. 如果 `dir_link_util_wall::<dir> > 85%` 且 `egress_residual_backlog_cycles_by_dir::<dir> / wall_cycles > 0.08`，则该方向可能已接近带宽上限。
3. 如果 `local_link_util_wall::local0` 与 `::local1` 长期失衡，并且 `local_egress_flits` 同向失衡，则本地端口存在热点倾斜，需复查 local 映射策略。
4. 如果 `voq_full_events / wall_cycles` 持续抬升，先确认 `voq_depth_mode_per_ingress` 是否一致，再做跨实验比较；必要时调大 `voq_depth` 做 A/B。
5. 如果 `voq_avg_depth_by_egress_wall::<port> > 0.6 * voq_depth`，再下钻 `voq_avg_depth_by_egress_channel(_ingress)`，定位具体 channel/ingress 热点来源。
6. 如果 `egress_stall_cycles_by_dir::<dir> / wall_cycles > 0.1`，继续拆分：`egress_no_credit_cycles_by_dir` 高说明 credit 回流慢，`egress_no_send_other_cycles_by_dir` 高说明仲裁/端口冲突问题更大。
7. 如果 `hop_count_hist_req` 主峰右移且 `e2e_latency_hist_req` 的 p99/p50 同步变差，则优先判断“路径变长”；若 hop 稳定但 e2e 变差，则优先判断“排队拥塞”。
8. 如果 `wait_compack_avg_cycles > 5` 且 `wait_compack_pending_max > 4`，或 `protocol_snp_total > 0`（当前版本），分别优先排查 ACK 回程拥塞与 snoop 触发路径覆盖度。

---

## 4. 备注与待补充项


| 项                                                           | 状态      |
| ----------------------------------------------------------- | ------- |
| `hnf_in_trans_latency_hist`（Phase3 完整版）                     | `[待补充]` |
| stats 对象真实层级路径（`\<mesh_path\>/\<bridge_path\>/\<hn_path\>`） | `[待补充]` |
| 各 workload 的阈值标定基线（如 `astar`/`mcf`）                         | `[待补充]` |
