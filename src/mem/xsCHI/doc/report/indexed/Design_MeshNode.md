# MeshNode（CMN-700 XP 风格）技术方案（草案）

目标：在 `xsCHI` 中新增 `MeshNode`，作为二维网格拓扑的基础路由单元，承担类似 CMN-700 Crosspoint（XP）的核心职责；在不破坏当前 `L2Wrapper → CHIBridge → FakeL3 → DDRWrapper` 事务语义的前提下，把“协议处理端点”和“网络路由节点”解耦。

---

## 范围与约束


| 维度   | 设计范围（本方案）                              | 当前约束（来自 xsCHI 现状）                                       |
| ---- | -------------------------------------- | ------------------------------------------------------- |
| 角色定位 | `MeshNode` 只做 flit 路由/仲裁/流控，不终止 CHI 事务 | 事务终止与协议语义仍由 `CHIBridge/FakeL3/DDRWrapper` 负责            |
| 协议通道 | 支持 CHI 四通道：`REQ/RSP/SNP/DAT` 的统一转发路径   | 现网 `SNP` 业务逻辑未实现，先保证“可转发/可阻塞”，不上层语义                     |
| 路由算法 | 默认 `XY`（先 X 后 Y），保证简单无死锁               | `NodeID` 编码固定，坐标位宽与端口位由现有实现决定                           |
| 流控机制 | 复用 `CHIPort` 现有 credit + backpressure  | 不额外重写链路层 credit 协议                                      |
| 端口模型 | 目标形态：4 个 mesh 方向 + 本地设备端口              | 现阶段优先支持 `local0/local1 + east/west/north/south` 六端口固定模型 |
| 配置形态 | 参数化配置（Python SimObject）替代真实 CMN 寄存器编程  | 暂不建模 CMN-700 64KB XP 寄存器空间                              |
| 兼容性  | 可在单节点/双节点场景退化为“直连转发器”                  | 需兼容现有 TopoSys 的最小链路，不破坏已有测试                             |


---

## 组件接口设计

### 1. 新增类与文件


| 类型               | 建议路径                               | 作用                            |
| ---------------- | ---------------------------------- | ----------------------------- |
| C++ 头文件          | `src/mem/xsCHI/device/MeshNode.hh` | 声明 MeshNode 接口、路由与仲裁状态        |
| C++ 实现           | `src/mem/xsCHI/device/MeshNode.cc` | 实现接收回调、路由计算、出端口调度             |
| Python SimObject | `src/mem/xsCHI/device/MeshNode.py` | 参数定义与 gem5 对象导出               |
| 构建脚本             | `src/mem/xsCHI/device/SConscript`  | 注册 `MeshNode` 的 SimObject 与源码 |


### 2. 类职责（与现有模块边界）


| 模块                   | 主要职责             | 与 MeshNode 的关系                  |
| -------------------- | ---------------- | ------------------------------- |
| `CHIBridge`          | RN 侧请求封装与响应回收    | 作为 MeshNode 的本地接入设备（local port） |
| `FakeL3`（后续可替换为 HNF） | HN 侧事务转换与回程映射    | 作为 MeshNode 的本地接入设备（local port） |
| `DDRWrapper`         | DRAM 事务提交与数据回传   | 通常挂在远端 MeshNode 的 local port    |
| `CHIPort`            | 通道缓冲、credit、回调机制 | MeshNode 的唯一链路收发接口              |
| `MeshNode`           | 路由+仲裁+转发         | 不改变 flit 语义字段，只决定下一跳            |


### 3. `MeshNode` 参数建议


| 参数                           | 类型         | 说明                 | 默认建议       |
| ---------------------------- | ---------- | ------------------ | ---------- |
| `node_x`                     | `Unsigned` | 本节点 X 坐标           | 必填         |
| `node_y`                     | `Unsigned` | 本节点 Y 坐标           | 必填         |
| `port_local0`                | `CHIPort`  | 本地设备端口 0           | 必填         |
| `port_local1`                | `CHIPort`  | 本地设备端口 1           | 可选         |
| `port_east/west/north/south` | `CHIPort`  | 四个方向网格端口           | 边界节点可缺省    |
| `enable_nonxy`               | `Bool`     | 是否启用非 XY 覆盖路由      | `False`    |
| `arb_policy`                 | `String`   | 仲裁策略（如 `fixed_rr`） | `fixed_rr` |
| `voq_depth`                  | `Unsigned` | 每个输出方向每通道排队深度      | `8`        |


### 4. 内部状态建议


| 状态                              | 说明                               |
| ------------------------------- | -------------------------------- |
| `std::array<CHIPort*, 6> ports` | 固定六端口指针表（local0/local1/e/w/n/s）  |
| `out_voq[dst_port][channel]`    | 每输出端口每通道的待发队列（保存 `FlitPtr`）      |
| `rr_cursor[dst_port][channel]`  | 仲裁轮询指针，避免输入饥饿                    |
| `route_override_table`          | 非 XY 覆盖项（可选）                     |
| `send_event`                    | 每周期调度重试事件（`CHIPort::send` 失败后重发） |
| `stats`                         | 收发 flit 计数、阻塞计数、按方向负载统计          |


---

## 路由与转发映射

### 1. NodeID/坐标解码（适配当前 xsCHI）

`NodeID` 现实现等价编码（仅使用端口 bit2）：

- `coords = tgt_id >> 3`
- `tgt_x = coords >> mesh_coord_bits`
- `tgt_y = coords & ((1 << mesh_coord_bits) - 1)`
- `tgt_local_port = (tgt_id & 0b100) ? 1 : 0`

> 说明：`mesh_coord_bits` 在当前 `NodeID.hh` 中为 5，可支持最多 32x32 网格。  
> MeshNode 路由只依赖 `tgt_id`，不需要访问 SAM。

### 2. 默认 XY 路由规则


| 条件                                 | 下一跳输出端口         | 备注                    |
| ---------------------------------- | --------------- | --------------------- |
| `tgt_x > cur_x`                    | `East`          | 沿 X 正方向               |
| `tgt_x < cur_x`                    | `West`          | 沿 X 负方向               |
| `tgt_x == cur_x && tgt_y > cur_y`  | `North`         | 沿 Y 正方向               |
| `tgt_x == cur_x && tgt_y < cur_y`  | `South`         | 沿 Y 负方向               |
| `tgt_x == cur_x && tgt_y == cur_y` | `Local0/Local1` | 由 `tgt_local_port` 选择 |


### 3. 非 XY 覆盖（对应 CMN700 XP 的可选能力）


| 项    | v1 决策                               |
| ---- | ----------------------------------- |
| 表项格式 | `src(x,y), tgt(x,y), mode(y_first)` |
| 生效条件 | `enable_nonxy = true` 且匹配表项         |
| 实现范围 | 只支持“首转向覆盖”（例如先 Y 后 X）               |
| 死锁约束 | v1 不自动求解全局死锁规则；仅允许离线检查后配置           |


### 4. 接收与转发时序


| 阶段          | 动作                                | 成功条件                         | 失败处理                             |
| ----------- | --------------------------------- | ---------------------------- | -------------------------------- |
| Ingress     | 入端口回调 `handleIngress(port, flit)` | 计算到合法 egress，且 egress VOQ 未满 | 返回 `false`，让上游 `CHIPort` 保留 flit |
| Enqueue     | `flit` 入 `out_voq`                | 入队成功                         | 维持 backpressure                  |
| Schedule    | 每周期扫描可发队列并仲裁                      | 选择一个可发 flit                  | 若无可发，等待下周期                       |
| Egress send | 调用 `egress_port->send(flit)`      | `send=true`                  | `send=false` 时 flit 保留队首，重试      |
| Credit loop | 对端消费后返还信用                         | 信用恢复                         | 自动由 `CHIPort` 完成                 |


---

## 仲裁与流控策略

### 1. 通道优先级（与当前 CHIPort 行为对齐）

当前 `CHIPort` 的接收处理顺序为 `SNP > RSP > DAT > REQ`。  
为保持一致，MeshNode 推荐默认采用同序优先级，然后在每个通道内使用 RR。


| 层次           | 策略                           |
| ------------ | ---------------------------- |
| 跨通道          | `SNP > RSP > DAT > REQ` 固定优先 |
| 通道内多输入竞争同一输出 | `Round-Robin`（保存游标）          |
| 同输入多候选（可选）   | 单 flit 单目的，无需额外仲裁            |


### 2. 与 CMN700 XP 特性映射


| CMN700 XP 特性                 | MeshNode v1 映射策略                           | 备注                      |
| ---------------------------- | ------------------------------------------ | ----------------------- |
| Credit-based flow control    | 直接复用 `CHIPort` credit 机制                   | 已具备                     |
| RX buffer 可配深度               | 用 `CHIPort.recv_buffer_size` + `voq_depth` | 可逐步统一                   |
| 双通道（2xREQ/2xRSP/2xSNP/2xDAT） | 暂不实现                                       | 预留参数位                   |
| DCS/MCS/CCS 切片               | 暂不显式建模                                     | 可用额外节点或延迟参数近似           |
| QoS 相关寄存器                    | 暂不实现                                       | 后续加入优先级权重               |
| RAS/校验                       | 暂不实现                                       | 先做 debug assert/counter |


---

## 与 xsCHI 现有架构的集成方式

### 1. 连接模型（阶段化）


| 阶段      | 拓扑形态                   | 目标                |
| ------- | ---------------------- | ----------------- |
| Phase-0 | 单 MeshNode（仅 local 端口） | 验证不破坏现有单链路功能      |
| Phase-1 | 2~4 个 MeshNode 小网格     | 打通 RN/HN/SN 跨节点路由 |
| Phase-2 | NxM 网格 + 边界/角落端口裁剪     | 与 CMN-700 XP 形态一致 |


### 2. 对现有代码的最小侵入点


| 文件                                  | 变更建议                                      |
| ----------------------------------- | ----------------------------------------- |
| `src/mem/xsCHI/device/SConscript`   | 注册 `MeshNode.py` + `MeshNode.cc`          |
| `src/mem/xsCHI/TopoSys/L2todram.cc` | 用 MeshNode 替换直接 `CHIPort::connect` 的点对点连接 |
| `src/mem/xsCHI/doc/report/README.md` | 增补“拓扑层（MeshNode）”并重画数据路径                  |
| `src/mem/xsCHI/test/`               | 新增 MeshNode 路由与拥塞回压测试                     |


---

## 行为对比与需实现细项


| 功能项         | 当前状态             | MeshNode v1 要求 | 验收标准                                 |
| ----------- | ---------------- | -------------- | ------------------------------------ |
| 基础 XY 路由    | 无专用路由器           | 必须实现           | 定向流量按坐标到达目标                          |
| 本地端口下行      | 无                | 必须实现           | 同坐标流量正确送达 local0/local1              |
| 回压传播        | 仅链路级             | 必须实现           | 下游阻塞时上游 `CHIPort` 可观测持续 backpressure |
| 通道仲裁        | 仅 `CHIPort` 接收顺序 | 必须实现           | 多输入争用同输出不丢包、不饿死                      |
| 非 XY 覆盖     | 无                | 可选（建议预留）       | 可配置单条覆盖规则并生效                         |
| 双通道并行       | 无                | 不实现            | 文档中明确“后续扩展”                          |
| QoS/PMU/RAS | 无                | 不实现            | 提供基础计数器即可                            |


---

## 开发步骤（建议先后）


| 步骤            | 产出                        | 关键实现点                  | 风险                 |
| ------------- | ------------------------- | ---------------------- | ------------------ |
| 1. 搭骨架        | `MeshNode.hh/.cc/.py` 可编译 | 六端口建模、回调注册、基本参数        | 端口空指针/边界节点处理       |
| 2. 做路由        | `routeFor(flit)`          | NodeID 解码 + XY 规则      | NodeID 编码假设变化导致误路由 |
| 3. 做缓冲与仲裁     | `out_voq + scheduler`     | VOQ 入队、RR 选择、`send` 重试 | 队列饥饿与重试风暴          |
| 4. 接入 TopoSys | 小网格可运行                    | 替换直接 connect，完成端点挂接    | 初期排障复杂度上升          |
| 5. 补测试        | 单元+集成用例                   | 路由正确性、回压、突发流量          | 测试覆盖不足导致回归         |


---

## 测试计划


| 测试层级 | 用例                                                        | 预期                        |
| ---- | --------------------------------------------------------- | ------------------------- |
| 单元测试 | `routeFor()`：四方向 + 本地下行                                   | 输出方向与坐标关系一致               |
| 单元测试 | VOQ 满队列行为                                                 | 入端口回调返回 `false`，上游不丢 flit |
| 单元测试 | RR 仲裁                                                     | 多输入长期竞争时无饥饿               |
| 集成测试 | `L2Wrapper -> MeshNode -> FakeL3`                         | 读写事务与现有基线一致               |
| 集成测试 | 多跳 `L2 -> Mesh -> Mesh -> DDR`                            | 往返延迟增加但语义正确               |
| 回归测试 | 现有 `testNodeID/testSAM/testTxnManager` + 新增 MeshNode case | 不破坏现有模块                   |


---

## 讨论点与当前决策


| 议题                    | 当前决策        | 原因                          |
| --------------------- | ----------- | --------------------------- |
| 是否一开始实现完整 CMN700 寄存器集 | 否           | 先把可运行拓扑打通，减少建模复杂度           |
| 是否立即支持双通道             | 否           | 当前 `CHIPort` 为单实例四通道，先保证正确性 |
| 是否支持 6 个设备端口（P0-P5）   | 否（v1 先 2 个） | 现有 NodeID/端点模型主要围绕 2 个本地口   |
| 是否支持非 XY              | 预留接口，默认关闭   | 满足未来热点绕行诉求                  |
| 是否替换 FakeL3           | 不在本任务       | 本任务聚焦“路由节点”，不改事务终止端         |


---

## 后续改进建议


| 优先级 | 建议                                        | 价值              |
| --- | ----------------------------------------- | --------------- |
| 高   | 在 `README.md` 增加“端点层/拓扑层”分层图              | 降低后续维护者理解成本     |
| 高   | 给 MeshNode 增加 debug flag（如 `CHIMeshNode`） | 快速定位路由与阻塞问题     |
| 中   | 为 non-XY 提供离线合法性检查脚本                      | 避免死锁配置          |
| 中   | 引入基础 PMU 计数（按端口/通道）                       | 支撑性能分析          |
| 低   | 逐步补齐 QoS/多通道建模                            | 对标 CMN-700 高级特性 |

