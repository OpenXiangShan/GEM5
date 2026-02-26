# CHI 接入 CacheWrapper 技术方案（草案）

目标：用真实 gem5 `CacheWrapper` 取代当前 FakeL3 的内部处理，使其成为 CHI 的 HN/缓存代理。CHI 侧收/发 flit，转换为 gem5 cache 请求/响应，复用现有 `CacheWrapper` 行为。

## 范围与约束
- 覆盖 FakeL3 现有职责：读/写/驱逐路径、Txn 追踪、数据分片、Comp/CompAck/DBIDResp 处理与转发。
- 不引入 snoop（保持未实现，明确断言/占位）。
- 复用现有 L2Wrapper、CHIBridge、DDRWrapper，不修改其对外接口；在 FakeL3 位置挂接新组件（暂称 CHI-CacheWrapper）。
- 兼容现有参数/NodeID/SAM 配置与信用流控（`CHIPort`）。

## 组件接口设计
- **新类**：`xsCHI::CacheWrapperCHI`（命名可再议），继承 `ClockedObject`，持有：
  - 两个 `CHIPort`：`cpuSidePort`（接 L2Wrapper）、`memSidePort`（接 DDRWrapper）。
  - 一个 `CacheWrapper` 实例或指针（通过参数创建）。
  - Txn 追踪表：从 CHI TxnID → `Request`/状态；从 CacheWrapper 侧 pkt/req → CHI 会话元数据。
- **对外连接**：保持与 FakeL3 相同的 CHI 端口数量与缓冲参数，保证可无缝替换。

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

3) **HN → DRAM 下行**
   - 当 CacheWrapper miss 或需要写下行，透传到 memSidePort→DDRWrapper。
   - 写数据分片：收到 L2 的 `COPYBACKWRDATA` → CacheWrapper 写 path（或直通 DRAM）→ 完成后 `NCBWRDATACOMPACK`/`COMPDBIDRESP`。

4) **DRAM → HN → L2 上行**
   - DDRWrapper 返回 `DBIDRESP`/`COMPDATA`，会话表定位源 L2，转封成对应 opcode（与 FakeL3 相同）。

## 行为对比与需要实现的细项
- **会话表**：
  - `txnTable`: CHI TxnID → {op, addr, size, srcId, returnNid, returnTxnid, dbid, data_bitmap, pkt_ptr?}。
  - `cacheReqMap`: CacheWrapper reqID/pkt* → txn metadata；便于响应回填 CHI。
- **数据分片**：
  - 复用 `Request::generateWriteDataID/finishTransferdata/gatherDataFlit` 管理位图。
  - 读数据从 CacheWrapper 回来后按 `DATA_TRANSFER_WIDTH` 切片生成 flit。
- **Opcode 映射表**：
  - CHI→gem5：READUNIQUE→ReadEx；READSHARED→ReadShared；READCLEAN→ReadClean；WRITEBACKFULL→WritebackDirty；WRITECLEANFULL→WritebackClean；EVICT→CleanEvict；CLEANUNIQUE→UpgradeReq/CleanUnique 语义。
  - gem5→CHI：miss 读返回 COMPDATA(+COMPACK)；写回完成返回 DBIDRESP→COMPDBIDRESP；驱逐返回 COMP。
- **信用与调度**：
  - 继续使用 `CHIPort` 的 BUFFER/credit 机制；回调需在处理失败时保持 flit 留在 buffer。
- **错误/未实现**：
  - Snoop、DVM 仍未实现，收到则 panic/assert。

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
