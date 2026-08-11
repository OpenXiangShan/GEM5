# CHI 接入 CacheWrapper 技术方案（草案）

目标：用真实 gem5 `CacheWrapper` 取代当前 FakeL3 的内部处理，使其成为 CHI 的 HN/缓存代理。拓扑更新为 `CHIPort ⇆ coherent_xbar ⇆ CacheWrapper ⇆ CHIPort`，在 coherent_xbar 中完成一致性维护与转发。CHI 侧收/发 flit，转换为 gem5 cache 请求/响应，复用 `coherent_xbar + CacheWrapper` 行为。最终类名：`CHI_L3`。

## 范围与约束
- 覆盖 FakeL3 现有职责：读/写/驱逐路径、Txn 追踪、数据分片、Comp/CompAck/DBIDResp 处理与转发。
- 不引入 snoop（保持未实现，明确断言/占位）。
- 复用现有 CHI_L2、CHIBridge、DDRWrapper，不修改其对外接口；在 FakeL3 位置挂接新组件（暂称 CHI-CacheWrapper）。
- 兼容现有参数/NodeID/SAM 配置与信用流控（`CHIPort`）。

## 组件接口设计（更新）
- **新类**：`xsCHI::CHI_L3`，继承 `ClockedObject`，持有：
  - 两个 `CHIPort`：`cpuSidePort`（接 CHI_L2）、`memSidePort`（接 DDRWrapper），缓冲/credit 仍沿用 CHIPort 参数。
  - 一个 `coherent_xbar` 实例与一个 `CacheWrapper` 实例，均通过参数传入现有对象，保持原生 gem5 Request/Response 端口连接关系。
  - Txn 追踪表：从 CHI TxnID → `Request`/状态；从 coherent_xbar/CacheWrapper 侧 pkt/req → CHI 会话元数据。
- **对外连接**：保持与 FakeL3 相同的 CHI 端口数量与缓冲参数，`cpuSidePort` 连接 RN，`memSidePort` 连接 DDRWrapper，内部新增 coherent_xbar。

## 事务/数据流映射
1) **L2 → HN (读)**
   - 收到 REQ 类 flit（`READUNIQUE/READSHARED/READCLEAN`）。
   - 为该 Txn 建立会话：记录 `SrcId/TxnId/Dbid/ReturnNid` 等。
   - 生成 gem5 `Packet`：命令映射到 ReadEx/ReadShared/ReadClean，addr/size 同步，Txn 元数据存入 side-table。
   - 通过内部 `CacheWrapper` 的 mem-side接口提交（需支持异步 resp）。
   - CacheWrapper 返回响应数据：封装为 `CHI_DAT_COMPDATA`（或分离数据的 RESP+DATA），`TxnId` 使用返还方的 ReturnTxnid/Dbid 规则与 FakeL3 一致；随后发送 `COMP`/`COMPACK` 完成。

2) **L2 → HN (写回 / 清除)**
   - REQ `WRITEBACKFULL/WRITECLEANFULL`：缓存写入数据，向 CacheWrapper 发送写/clean pkt；等待 CacheWrapper 完成后返回 `DBIDRESP` → L2 侧（保持现有时序）。
   - REQ `EVICT`：转为 clean op 或直接 ACK，匹配现有 FakeL3 行为（返回 `COMP`).
   - REQ `CLEANUNIQUE`：转为 Upgrade/Clean 请求，返回 `COMP` + `Dbid`，后续写数据流与 FakeL3 一致。

3) **HN → coherent_xbar → CacheWrapper → DRAM 下行**
  - CHI 请求翻译为 gem5 `Packet` 后送入 `coherent_xbar`，与 CacheWrapper 按原生 gem5 Request/Response 端口连接。
  - 在 CHI_L3 内伪造 gem5 端口：伪 mem-side 口接 xbar CPU-side；伪 CPU-side 口接 CacheWrapper mem-side，用于拦截流量。
  - xbar 命中：在伪 mem-side 口截获 xbar CPU-side 发出的 resp，转成 CHI `DAT_COMPDATA`/`COMP`/`COMPACK` 上行。
  - xbar miss：在伪 CPU-side 口截获 CacheWrapper mem-side 下行 req，转成 CHI `REQ_READNOSNP` / `REQ_WRITENOSNPFULL` 发送 DDRWrapper，并在 `downstreamMap` 记录“下行 pkt → CHI txn”以确保回包匹配。涉及数据的读/写/写回才转换，其他类型先断言。
  - 写数据分片：收到 L2 的 `COPYBACKWRDATA`，缓存数据并在 CacheWrapper 写完成后生成 `NCBWRDATACOMPACK`/`COMPDBIDRESP` 等。

4) **DRAM → CacheWrapper → coherent_xbar → HN → L2 上行**
  - DDRWrapper 返回 `DBIDRESP`/`COMPDATA`，先等待数据接收完成，查下行映射表找到原 CacheWrapper 请求，构造数据 response pkt 传回 CacheWrapper（而非直接回 L2）。
  - L2 上行由后续拦截 xbar CPU-side 发出的 resp 完成（保持 CHI DAT/COMP/COMPACK 生成逻辑）。

## 行为对比与需要实现的细项（更新）
- **会话表/映射表**：
  - `txnTable`: CHI TxnID → {op, addr, size, srcId, returnNid, returnTxnid, dbid, data_bitmap, pkt_ptr?}。
  - `cacheReqMap`: coherent_xbar / CacheWrapper 发出的 pkt* → txn metadata；命中路径回封 CHI。
  - `downstreamMap`: CacheWrapper mem-side 下行 pkt* → CHI txn metadata；用于 DRAM 回包后重建 response pkt 返给 CacheWrapper。
  - 清理：当拦截到 xbar 上行 resp pkt 时，通过 `cacheReqMap` 找到对应 txn，待该 txn 按完成判定结束后再删除映射（含 cacheReqMap/downstreamMap/txnTable）。
- **伪端口与阻塞/重试语义**：
  - CHI_L3 内自定义派生的 Request/ResponsePort 适配层（内侧 req/resp 伪端口），阻塞/重试语义与 classic cache Request/ResponsePort 相同。
  - 若 coherent_xbar 阻塞/拒收，则在 CHI_L3 内保存该请求并下一拍重试；不必与 CHIPort 的重试事件对齐。
  - `downstreamMap` 以 pkt* 为键，需持有 pkt 生命周期的引用/智能指针；同地址多 outstanding 若导致悬空或冲突则直接 panic。
- **拦截点**：
  - CHI→xbar：聚合完 txn flit 后生成 pkt，送 xbar。
  - xbar→上行（命中）：通过 CHI_L3 重写的 RequestPort 连接 coherent_xbar cpu_side_ports[0]，在 recvTimingResp 中截获 pkt，转为 CHI DAT/COMP/COMPACK。
  - xbar→下行（miss）：通过 CHI_L3 重写的 ResponsePort 连接 coherent_xbar mem_side_ports[0]，在 recvTimingReq 中截获 CacheWrapper 向下的读/写 pkt，转为 CHI READ/WRITE 发往 DDRWrapper。
- **数据分片**：
  - 复用 `Request::generateWriteDataID/finishTransferdata/gatherDataFlit` 管理位图。
  - 读数据从 CacheWrapper 回来后按 `DATA_TRANSFER_WIDTH` 切片生成 flit。
  - 假定 DATA_TRANSFER_WIDTH=256b、行 64B，不匹配则断言；COMPDATA 不重排（假定顺序或无需重排）。
- **Opcode 映射表**：
  - CHI→gem5：READUNIQUE→ReadEx；READSHARED→ReadShared；READCLEAN→ReadClean；WRITEBACKFULL→WritebackDirty；WRITECLEANFULL→WritebackClean；EVICT→CleanEvict；CLEANUNIQUE→UpgradeReq/CleanUnique 语义。
  - gem5→CHI：miss 读返回 COMPDATA(+COMPACK)；写回路径 REQ_WRITENOSNPFULL：先 DBIDRESP，再收全分片后 COMPDBIDRESP；CLEANUNIQUE 反向映射，固定 COMP+Dbid（不改动 Packet 内容，但需有可用 dbid）；驱逐返回 COMP。仅支持 COMPDATA 全量返回，未对齐/部分写断言。
- **信用与调度**：
  - 继续使用 `CHIPort` 的 BUFFER/credit 机制；回调需在处理失败时保持 flit 留在 buffer。
  - `CHIPort` send 失败按 CHIBridge/FakeL3 模式用事件重试；coherent_xbar backpressure 需自管重试队列和优先级。
  - REQ_WRITENOSNPFULL 写数据分片：受 CHIPort credit/时序限制分批发送；发送失败在 CHI_L3 内维护有序队列，下一拍重试。
- **错误/未实现**：
  - Snoop、DVM 未实现，收到 panic/assert。
  - 非 cacheable/IO 请求断言（应在 L2 已分流）。未支持 opcode、异常数据大小、跨行访问：panic/assert。映射表缺失（乱序/重复返回）panic。

- **Txn 完成判定（对齐 FakeL3 行为）**：
  - 读：收到全部 COMPDATA 分片并完成必要 COMP/COMPACK 序列。
  - 写回：REQ_WRITENOSNPFULL 路径，先 DBIDRESP，再全部写数据分片完成后 COMPDBIDRESP；CLEANUNIQUE：COMP+Dbid 返回即完成；EVICT 收到 COMP。
  - 以 FakeL3::handleDramsideRecv 的分支行为为准，完成后清理 txnTable/cacheReqMap/downstreamMap。
  - 断言触发直接 panic，不做额外清理。

## 开发步骤（建议先后）
1. **骨架类**：新增 `CacheWrapperCHI`，参数含两端 CHI port、内部 CacheWrapper 对象引用/创建方式，注册 receive_callback。
2. **会话状态结构**：定义 txn/req 状态记录（含数据位图）。
3. **RX 处理 (L2→HN)**：实现 REQ 路径映射、创建 pkt、投递到 CacheWrapper；写回数据缓存。
4. **CacheWrapper 回调→CHI**：实现数据返回/完成信号转换为 flit（COMPDATA/RESPSEPDATA+DATASEPRESP/COMP/COMPACK/DBIDRESP）。
5. **DRAM 侧 RX/TX**：复刻 FakeL3 的 DRAM 侧处理，用新状态表驱动。
6. **时序与重试**：确保 `CHIPort::send` 失败时保留 flit，调度重试事件；CacheWrapper 若 backpressure，按其端口语义处理。
7. **测试计划**：
   - 单元：TxnID 管理、分片位图、opcode 映射。
   - 集成：L2→CacheWrapperCHI→DDRWrapper 全链路读写；写回带数据；驱逐；分离数据路径。
   - 回归：保持 FakeL3 行为等价（在无 snoop 情况下）。

## 讨论点与最新决定
- **CacheWrapper 接口保持不变**：不在 CacheWrapper 内新增字段，所有 CHI 元数据放在外部 side map（Txn table / pkt map）中维护。
- **写回/事务处理时序**：
  - L2 来的事务先按 Txn 聚合全部 flit，收到完整事务后再生成对应 CacheWrapper 请求。
  - CacheWrapper 侧所有向上/向下的请求都被拦截并转换：
    - Miss 下行读：拦截并转成 FakeL3 原有的 CHI 请求流向 DDRWrapper。
    - Hit 上行返回：拦截并按记录的 Txn 元数据转换为规范的 CHI 响应流程返回上层。
  - DBIDRESP/COMP 等时序遵循 FakeL3 现行路径，但对齐 CHI-E 规范。
- **数据返回形态**：初期仅支持 COMPDATA 全返回，不实现 RESPSEPDATA/DATASEPRESP。

## 回复规则清单（需按此实现）

- **RN → L3 (cpuSidePort) 收到的 CHI Flit 与期望回复**
  - Request 类：
    - `REQ_READUNIQUE/REQ_READSHARED/REQ_READCLEAN`: 构造一个 packet（gem5 命令对应 ReadExReq/ReadSharedReq/ReadCleanReq）通过伪造端口送入 xbar，同时记录 txn 及伪造的 packet 以便命中路径回封；miss 时下发 `REQ_READNOSNP` 到 DDR，收齐数据后上行 `DAT_COMPDATA` 全分片 + `COMPACK`；命中则直接生成对应 DAT/COMP/COMPACK。
    - `REQ_WRITEBACKFULL/REQ_WRITECLEANFULL`: 构造一个 packet（WritebackDirty/WritebackClean）通过伪造端口送入 xbar；写回无上行 resp，故不记录 txn/packet；下发 `REQ_WRITENOSNPFULL` 到 DDR，等待 `DBIDRESP` 后按分片发送写数据，全部完成后返回 `COMPDBIDRESP`。
    - `REQ_CLEANUNIQUE`: 构造一个 packet（UpgradeReq 语义）通过伪造端口送入 xbar，并记录 txn+packet；返回 `COMP` 携带 dbid（与 CHIBridge 反向转换一致），完成条件为 COMP 收到。
    - `REQ_EVICT`: 构造一个 packet（CleanEvict）通过伪造端口送入 xbar，直接返回 `COMP`，不记录 packet（无后续 resp）。
    - 其他 opcode：panic/assert。
  - CHI_CHN_TYPE_RSP：查保存的请求，若为 `REQ_READUNIQUE/REQ_READSHARED/REQ_READCLEAN` 且收到 `CHI_RSP_COMPACK`，标记该 txn 完成并释放相关映射/资源。
  - 拦截 xbar 上行 resp：通过 resp packet 的指针查映射的 CHI 请求；若 resp 为 ReadExResp/ReadSharedResp/ReadCleanResp，分别断言映射请求为 `REQ_READUNIQUE/REQ_READSHARED/REQ_READCLEAN`，提取数据后开始发送 `DAT_COMPDATA`；其他 resp 先 panic。
  - 拦截 CacheWrapper 下行 req：若 req 为 ReadExReq/ReadSharedReq/ReadCleanReq，构建 `CHI_REQ_READNOSNP` 并据 req 对应的 CHI 请求填 txn_id，发送给 DDR，同时保存该 req 以便数据回填后回复 CacheWrapper；若 req 为 WritebackDirty/WritebackClean，构建 `CHI_REQ_WRITENOSNPFULL`，申请 txn 并 saveOutstandingRequest，填字段后发送 DDR。

- **DDR → L3 (memSidePort) 收到的 CHI Flit 与处理**
  - `RSP_DBIDRESP`（对应 WRITENOSNPFULL）：查 downstreamMap，转为 `RSP_COMPDBIDRESP` 上行（经 CacheWrapper/xbar 路径），并驱动后续数据发送完成判断。
  - `DAT_COMPDATA`: 查 downstreamMap/txnTable，按分片标记完成，组装为 CacheWrapper 的数据 response pkt，后续由 xbar 命中拦截生成上行 DAT/COMP/COMPACK。
  - 其他 opcode：panic/assert。

## 端口绑定与 DMT 假设（新增）
- coherent_xbar 绑定：CHI_L3 重写的 RequestPort（InnerCacheReqPort）连接 coherent_xbar 的 cpu_side_ports[0]；重写的 ResponsePort（InnerCacheRespPort）连接 coherent_xbar 的 mem_side_ports[0]。
- DMT：当前 CHI_L3→DDRWrapper 不使用 DMT，下发 `REQ_READNOSNP`/`REQ_WRITENOSNPFULL` 时仅设置 TgtId（来自 SAM），TxnId 自分配，不填 ReturnNid/ReturnTxnid。后续计划在 DDRWrapper 增加 DMT 模式开关；开启后按 DMT 返还给 ReturnNid/ReturnTxnid，未开启则直接返 HN(TgtId)。
