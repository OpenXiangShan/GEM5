# xsCHI 模块代码结构说明

本文档总结 `src/mem/xsCHI` 下自定义 CHI 互连模型的主要组成、职责与数据流，便于快速定位代码与扩展。

## 当前支持的 CHI 事务类型
- 读类：`READUNIQUE`、`READSHARED`、`READCLEAN`（由 L2Wrapper 从 ReadEx/ReadShared/ReadClean/硬件预取映射）
- 写回类：`WRITEBACKFULL`、`WRITECLEANFULL`（L2 写回脏/干净）
- 维护类：`EVICT`、`CLEANUNIQUE`（UpgradeReq 映射到 CLEANUNIQUE）
- 转换/下行到 DRAM 时的内部 opcode：`READNOSNP`、`WRITENOSNPFULL`（FakeL3 下发到 DRAM 侧）、分离数据相关 `RESPSEPDATA`/`DATASEPRESP`、写回数据 `COPYBACKWRDATA`、数据完成 `COMPDATA`、确认 `COMP`、`COMPACK`、`COMPDBIDRESP`、`DBIDRESP`

## 事务与各组件处理逻辑
- **L2Wrapper**
  - 将 gem5 `Packet` 映射为 `Request`：
    - `ReadEx/HardPF`→`READUNIQUE`；`ReadShared`→`READSHARED`；`ReadClean`→`READCLEAN`
    - `CleanEvict`→`EVICT`；`UpgradeReq`→`CLEANUNIQUE`
    - `WritebackDirty`→`WRITEBACKFULL`，`WritebackClean`→`WRITECLEANFULL`（复制数据载荷）
  - 调用 `CHIBridge::ReceiveReq` 发送到网络；记录需要响应的读请求以便回填 `Packet`。
  - 收到 `recvReadResp`（桥完成读）时，将数据拷回原 `Packet`、发送 timing 响应。

- **CHIBridge（RN 侧）**
  - 为入向请求分配 TxnID，生成 `Flit`（填充 `SrcId`、`TgtId` 来自 SAM），经 `CHIPort` 发送并挂起 outstanding。
  - 读路径：处理返回的 `DAT_COMPDATA` 或分离数据 `RESPSEPDATA`/`DATASEPRESP`，调用 `Request::gatherDataFlit` 聚合；数据与响应齐备后回调 L2Wrapper、释放 TxnID，并向对端发送 `COMPACK`（使用 `Dbid`/`HomeNid` 信息）。
  - 写回路径：收到 `COMPDBIDRESP` 后按 `generateWriteDataID` 逐片发送 `DAT_COPYBACKWRDATA`，全部片段完成后释放 TxnID。
  - 驱逐：`EVICT` 仅等待 `COMP` 即完成；`CLEANUNIQUE` 收到 `COMP` 即完成（并可能触发 `COMPACK`）。
  - Snoop 未实现，相关分支断言。

- **FakeL3（简化 HN）**
  - L2→HN：
    - 读 `READUNIQUE/READSHARED/READCLEAN` 下发为 `READNOSNP` 到 DRAM 侧（重新分配 TxnID，记录 ReturnNID/Txnid）。
    - 写回 `WRITEBACKFULL/WRITECLEANFULL` 下发为 `WRITENOSNPFULL`，保存源 txn 以便回程映射。
    - `EVICT` 直接回 `COMP` 给 L2；`CLEANUNIQUE` 生成 `COMP`（带新 Dbid）发往 DRAM 侧，等待后续 DBID 响应。
  - DRAM→L2：
    - 数据 `DAT_COMPDATA` 复制并转发给 L2，同时标记分片完成。
    - 对写回，收到 `DBIDRESP` 后转为 `COMPDBIDRESP` 送 L2，随后接收 L2 发来的写数据 `COPYBACKWRDATA`，转封为 `NCBWRDATACOMPACK` 下发 DRAM，全部分片完成后释放 txn。
    - 收到其他目的节点的 flit 会复用拷贝转发回 L2。

- **DDRWrapper（存储端）**
  - 接收来自 FakeL3 的 `READNOSNP`/`WRITENOSNPFULL` 等 flit，转换为 DRAMsim3 事务，维护 outstanding 表。
  - 读完成：将 DRAM 返回数据封装为 `CHI_DAT_COMPDATA`（携带原 ReturnNID/Txnid、HomeNid/Dbid），按 `DATA_TRANSFER_WIDTH` 分片发送；全部分片发送后清理 outstanding。
  - 写路径：在 `WRITENOSNPFULL` 进入时调用 `accessAndRespond`，当写完成后返回 `DBIDRESP` 给 FakeL3；写数据分片由 FakeL3 通过 `NCBWRDATACOMPACK` 下发，Wrapper 不再拆分。
  - 依赖 `TxnIDManager` 跟踪本端事务，`responseQueue` 按 tick 发送。

## 目录分层
- **base/**：协议基础类型与通用设施。
  - [base/FlitOpType.hh](base/FlitOpType.hh)：CHI 四条通道的全部 opcode 枚举与字符串转换。
  - [base/flit.hh](base/flit.hh) & [base/flit.cc](base/flit.cc)：`Flit` 数据单元，封装路由/事务字段、数据载荷管理、通道类型判断、拷贝/构造与数据拷贝接口。
  - [base/request.hh](base/request.hh) & [base/request.cc](base/request.cc)：`Request` 事务对象，携带 CHI 头字段、数据缓冲与传输状态位图，支持数据收集 (`gatherDataFlit`)、写数据分片 ID 分配、读响应克隆。
  - [base/CHIPort.hh](base/CHIPort.hh) & [base/CHIPort.cc](base/CHIPort.cc)：带信用与多通道缓冲的时钟化端口，负责发送信用检查、接收排队、逐通道回调处理及信用返还，内部按 SNP→RSP→DAT→REQ 优先级轮询。
  - [base/Network/NodeID.hh](base/Network/NodeID.hh)：网格坐标到 NodeID 的编码/解码（X/Y/Port → 3 位后缀）。
  - [base/Network/SystemAddressMap.hh](base/Network/SystemAddressMap.hh)：RN/HN 的地址映射，采用基于物理地址位的幂次 hashing 生成目标节点。
  - [base/Network/TxnManager.hh](base/Network/TxnManager.hh)：12-bit 事务 ID 分配器，支持占用检查与释放。
  - [base/params.hh](base/params.hh)：协议常量（数据宽度、缓冲规模等）。
- **device/**：具体设备/适配器。
  - [device/L2Wrapper.hh](device/L2Wrapper.hh) & [device/L2Wrapper.cc](device/L2Wrapper.cc)：Gem5 L2 与 CHI 的双向适配。CPU 侧 `CpuSidePort` 接收普通缓存请求，转为 `Request` 后交给 `CHIBridge`；未缓存请求透传到 mem 侧。收到桥返回的读响应后回填原始 `Packet` 并送回 CPU。
  - [device/CHIBridge.hh](device/CHIBridge.hh) & [device/CHIBridge.cc](device/CHIBridge.cc)：RN 侧桥。为请求分配 TxnID、生成 `Flit`、通过 `CHIPort` 发送，跟踪未完成事务；处理返回的 RSP/DAT，组装读数据、发送 CompAck，写回路径生成数据 flit；当前未实现 snoop。
  - [device/FakeL3.hh](device/FakeL3.hh) & [device/fakeL3.cc](device/fakeL3.cc)：简化的 HN/Fake L3。L2 侧收到读/写/驱逐请求后转换为无 snoop 的 DRAM 方向请求，维护 txn 映射；从 DRAM 侧收到数据/响应后转发给 L2 或驱动写入完成。
  - [device/DDRWrapper.hh](device/DDRWrapper.hh) & [device/DDRWrapper.cc](device/DDRWrapper.cc)：基于 `AbstractMemory` + DRAMsim3 的存储端。接收 `Flit`，将读写排队到 DRAMsim3，完成后按 CHI 数据 flit 回传，并维护 outstanding 读/写队列与响应优先队列。
  - 其他：`MeshNode.hh` 占位，`HNF.hh`（未在当前路径中实现）。
- **TopoSys/**：组合系统。
  - [TopoSys/L2todram.hh](TopoSys/L2todram.hh) & [TopoSys/L2todram.cc](TopoSys/L2todram.cc)：构建 L2→FakeL3→DDR 的最小系统，分配节点 ID，配置 SAM，并连接各 `CHIPort`。
- **test/**：gtest 单元测试。
  - [test/testNodeID.cc](test/testNodeID.cc)、[test/testSAM.cc](test/testSAM.cc)、[test/testTxnManager.cc](test/testTxnManager.cc) 覆盖 NodeID 编解码、SAM 目标选择与 TxnID 分配。

## 关键类与数据流
- **Flit / Request 生命周期**：
  - L2Wrapper 将 `Packet` 映射为 `Request`（读/写/驱逐/Upgrade 等），必要时复制数据载荷。
  - CHIBridge 为请求分配 TxnID、填充路由字段并封装为 `Flit`，通过 `CHIPort::send` 执行信用检查后发送。
  - FakeL3 / DDRWrapper 接收 flit 后，根据通道类型与 opcode 决定转发或落盘，并使用 `gatherDataFlit`、`generateWriteDataID` 等维护分片状态。
  - 完成条件：读需收齐 DATA + RSP（或 RespSepData），写需获得 CompDBIDResp 并发送全部写数据分片；完成后释放 TxnID 并从 outstanding 表删除。

- **端口与流控（CHIPort）**：
  - 四通道独立信用计数与上次发送周期，防止同周期二次发送。
  - 接收侧缓冲大小由参数 `recv_buffer_size` 控制，按通道优先级调度并在回调成功后返还信用。
  - `setReceiveCallback` 由拥有者（桥/缓存/DRAM）注册，实现协议处理入口。

- **地址与节点标识**：
  - NodeID 将 (X,Y,Port) 编成 3-bit 后缀，便于网格路由。
  - SystemAddressMap* 根据物理地址位 XOR 生成 HN 选择下标，支持 2 的幂数量的目标节点；RN/HN 版本逻辑相同，主要用于读写分发。

- **事务管理**：
  - TxnIDManager 维护 12-bit ID 的使用位图，保证并发事务上限可配置；CHIBridge/FakeL3/DDRWrapper 分别持有自己的实例。
  - Request 内部 `dataFlitsTransferred` 位图确保分片接收/发送计数准确，防止重复。

## 数据路径概览（L2 → DRAM）
1. CPU 发起缓存请求，`L2Wrapper::CpuSidePort::recvTimingReq` 转换为 `Request` 并调用 `CHIBridge::ReceiveReq`。
2. CHIBridge 分配 TxnID、创建 `Flit`，经 `CHIPort` 送往 FakeL3（HN）。
3. FakeL3 将读/写转换为无 snoop 的 DRAM 请求，使用自身 TxnID 管理，并把原始源/txn 记录在 outstanding。
4. DDRWrapper 将 flit 转为 DRAMsim3 事务；读完成后构造 `CHI_DAT_COMPDATA`，写路径在收到写请求时会直接 `accessAndRespond`，完成后返回 `CHI_RSP_DBIDRESP`，随后 FakeL3 触发数据下行与 CompDBIDResp 上行。
5. 数据/响应沿原路返回；CHIBridge 组装读数据后调用回调 `recvReadResp`，由 L2Wrapper 回填原 `Packet` 并送回 CPU。

## 当前缺口与扩展点
- Snoop 请求/响应未实现，`CHIBridge::handleNetworkPortReceive` 和 `FakeL3` 中相关分支直接断言。
- 信用/仲裁策略简单，暂未建模网络延迟与多节点拓扑，仅在 `CHIPort` 内做基本周期调度。
- `HNF.hh`、`MeshNode.hh` 为空壳，可按需要扩展真实 HN 行为或网格路由。

## 使用提示
- 需要最小可运行链路时，可使用 [TopoSys/L2todram.cc](TopoSys/L2todram.cc) 直接连接 L2Wrapper、FakeL3、DDRWrapper 并配置 SAM/NodeID。
- 添加新 opcode 或通道行为时，先更新 [base/FlitOpType.hh](base/FlitOpType.hh)，再在相关处理函数中补齐分支（L2Wrapper → CHIBridge → FakeL3/DDRWrapper）。
- 调试链路时可启用 `debug/CHIPort`, `debug/CHIBridge`, `debug/CHIL2Wrapper`, `debug/CHIFakeL3`, `debug/CHIDramsim` 等 trace 标志观察 flit 发送/信用状态。
