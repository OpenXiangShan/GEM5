#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "CHIBridge.hh"
#include "base/trace.hh"
#include "debug/CHIBridge.hh"
#include "debug/Cache.hh"

namespace gem5
{
namespace xsCHI
{
    uint64_t
    CHIBridge::blockAddr(uint64_t addr) const
    {
        return addr & ~static_cast<uint64_t>(0x3f);
    }

    bool
    CHIBridge::isReadReqOp(CHI_OP_TYPE op) const
    {
        return op == CHI_OP_TYPE::CHI_REQ_READUNIQUE ||
               op == CHI_OP_TYPE::CHI_REQ_READSHARED ||
               op == CHI_OP_TYPE::CHI_REQ_READCLEAN ||
               op == CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE ||
               op == CHI_OP_TYPE::CHI_REQ_EVICT;
    }

    bool
    CHIBridge::isWriteReqOp(CHI_OP_TYPE op) const
    {
        return op == CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL ||
               op == CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL;
    }

    bool
    CHIBridge::hasInProgressRead(uint64_t addr) const
    {
        const uint64_t blk = blockAddr(addr);
        auto it = inProgressReadByAddr.find(blk);
        return it != inProgressReadByAddr.end() && it->second > 0;
    }

    bool
    CHIBridge::hasInProgressWrite(uint64_t addr) const
    {
        const uint64_t blk = blockAddr(addr);
        auto it = inProgressWriteByAddr.find(blk);
        return it != inProgressWriteByAddr.end() && it->second > 0;
    }

    bool
    CHIBridge::hasQueuedReadWriteReq(uint64_t addr) const
    {
        const uint64_t blk = blockAddr(addr);
        std::queue<ReqPtr> pending = Req_tobesent;
        while (!pending.empty()) {
            const ReqPtr &queued_req = pending.front();
            if (queued_req && blockAddr(queued_req->getAddr()) == blk) {
                const CHI_OP_TYPE queued_op = queued_req->getOpcode();
                if (isReadReqOp(queued_op) || isWriteReqOp(queued_op)) {
                    return true;
                }
            }
            pending.pop();
        }
        return false;
    }

    void
    CHIBridge::trackReadStart(uint64_t addr)
    {
        const uint64_t blk = blockAddr(addr);
        inProgressReadByAddr[blk]++;
        DPRINTF(CHIBridge,
                "track read start blk=%#lx count=%u\n",
                blk, inProgressReadByAddr[blk]);
    }

    void
    CHIBridge::trackReadFinish(uint64_t addr)
    {
        const uint64_t blk = blockAddr(addr);
        auto it = inProgressReadByAddr.find(blk);
        if (it == inProgressReadByAddr.end()) {
            return;
        }
        if (it->second > 0) {
            it->second--;
        }
        DPRINTF(CHIBridge,
                "track read finish blk=%#lx count=%u\n",
                blk, it->second);
        if (it->second == 0) {
            inProgressReadByAddr.erase(it);
            wakeBlockedReqs(blk);
        }
    }

    void
    CHIBridge::trackWriteStart(uint64_t addr)
    {
        const uint64_t blk = blockAddr(addr);
        inProgressWriteByAddr[blk]++;
        DPRINTF(CHIBridge,
                "track write start blk=%#lx count=%u\n",
                blk, inProgressWriteByAddr[blk]);
    }

    void
    CHIBridge::trackWriteFinish(uint64_t addr)
    {
        const uint64_t blk = blockAddr(addr);
        auto it = inProgressWriteByAddr.find(blk);
        if (it == inProgressWriteByAddr.end()) {
            return;
        }
        if (it->second > 0) {
            it->second--;
        }
        DPRINTF(CHIBridge,
                "track write finish blk=%#lx count=%u\n",
                blk, it->second);
        if (it->second == 0) {
            inProgressWriteByAddr.erase(it);
            wakeBlockedReqs(blk);
        }
    }

    void
    CHIBridge::enqueueBlockedReq(ReqPtr req)
    {
        const uint64_t blk = blockAddr(req->getAddr());
        blockedReqByAddr[blk].push_back(req);
        DPRINTF(CHIBridge,
                "enqueue blocked req op=%s addr=%#lx blk=%#lx blocked=%u\n",
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(req->getOpcode()),
                req->getAddr(),
                blk,
                static_cast<unsigned>(blockedReqByAddr[blk].size()));
    }

    void
    CHIBridge::wakeBlockedReqs(uint64_t addr)
    {
        const uint64_t blk = blockAddr(addr);
        auto it = blockedReqByAddr.find(blk);
        if (it == blockedReqByAddr.end()) {
            return;
        }

        auto &q = it->second;
        // Preserve same-block request order: only wake one request at a time.
        if (!q.empty()) {
            Req_tobesent.push(q.front());
            q.pop_front();
        }
        const unsigned blockedLeft = static_cast<unsigned>(q.size());
        if (q.empty()) {
            blockedReqByAddr.erase(it);
        }
        DPRINTF(CHIBridge,
                "wake blocked reqs blk=%#lx req_queue=%u blocked_left=%u\n",
                blk,
                static_cast<unsigned>(Req_tobesent.size()),
                blockedLeft);
        if (!Req_tobesent.empty() && !req_handle_event.scheduled()) {
            scheduleReqRetry();
        }
    }
namespace
{
constexpr size_t kWaitCompAckHistBuckets = 256;
}

CHIBridge::BridgeStats::BridgeStats(CHIBridge *parent)
    : statistics::Group(parent, "protocol"),
      ADD_STAT(protocol_tx_by_opcode, statistics::units::Count::get(),
               "Protocol TX flits grouped by CHI opcode"),
      ADD_STAT(protocol_rx_by_opcode, statistics::units::Count::get(),
               "Protocol RX flits grouped by CHI opcode"),
      ADD_STAT(protocol_readshared_total, statistics::units::Count::get(),
               "Total ReadShared opcode observations"),
      ADD_STAT(protocol_writeevict_total, statistics::units::Count::get(),
               "Total WriteEvict opcode observations"),
      ADD_STAT(protocol_compack_total, statistics::units::Count::get(),
               "Total CompAck opcode observations"),
      ADD_STAT(protocol_snp_total, statistics::units::Count::get(),
               "Total snoop opcode observations"),
      ADD_STAT(wait_compack_cycles, statistics::units::Cycle::get(),
               "Accumulated waiting cycles for COMPACK before successful send"),
      ADD_STAT(wait_compack_cycles_hist, statistics::units::Cycle::get(),
               "Distribution of COMPACK waiting cycles"),
      ADD_STAT(wait_compack_sent_total, statistics::units::Count::get(),
               "Number of COMPACK flits successfully sent"),
      ADD_STAT(wait_compack_avg_cycles, statistics::units::Rate<
                    statistics::units::Cycle,
                    statistics::units::Count>::get(),
               "Average waiting cycles per COMPACK send"),
      ADD_STAT(wait_compack_pending_max, statistics::units::Count::get(),
               "Maximum pending COMPACK queue depth")
{
    using namespace statistics;

    protocol_tx_by_opcode
        .init(CHIBridge::NumChiOps)
        .flags(nozero);
    protocol_rx_by_opcode
        .init(CHIBridge::NumChiOps)
        .flags(nozero);
    for (size_t i = 0; i < CHIBridge::NumChiOps; ++i) {
        const auto op = static_cast<CHI_OP_TYPE>(i);
        const std::string label = CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op);
        protocol_tx_by_opcode.subname(i, label);
        protocol_rx_by_opcode.subname(i, label);
    }

    protocol_readshared_total.flags(nozero);
    protocol_writeevict_total.flags(nozero);
    protocol_compack_total.flags(nozero);
    protocol_snp_total.flags(nozero);
    wait_compack_cycles.flags(nozero);
    wait_compack_sent_total.flags(nozero);
    wait_compack_pending_max.flags(nozero);

    wait_compack_cycles_hist
        .init(kWaitCompAckHistBuckets)
        .flags(nozero | nonan);

    wait_compack_avg_cycles
        .flags(nozero | nonan)
        .precision(6);
    wait_compack_avg_cycles = wait_compack_cycles / wait_compack_sent_total;
}

    CHIBridge::CHIBridge(const Params &p)
        : ClockedObject(p),
          networkPort(p.networkPort),
          _NodeID(0),
          SAM(nullptr),
          TXN_Manager(1024),// default max outstanding transactions
          outstanding_requests(),
          stats(this),
          Req_tobesent(),
          req_handle_event([this] {
            DPRINTF(CHIBridge,"retry called ,Req_tobesent's number : %d reqOP:%s,addr:%lx\n",Req_tobesent.size(),
            CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(Req_tobesent.front()->getOpcode()),Req_tobesent.front()->getAddr());
            if (ReceiveReq(Req_tobesent.front(),true)){
                Req_tobesent.pop();
                if (!Req_tobesent.empty() ) {
                    assert(req_handle_event.scheduled() && "Req_handle_event should be scheduled");
                }else {
                    if(req_handle_event.scheduled()) {
                        DPRINTF(CHIBridge,"After handle, Req_tobesent become empty, deschedule event \n");
                        deschedule(req_handle_event);
                    }
                }
            }
        }, name()),
          Ack_tobesent(),
          ack_enqueue_ticks(),
          maxCompAckPending(0),
          ack_handle_event([this] { TrySendCompACK();}, name())

    {
        // 初始化存储端口和网络端口
        // storagePort->setReceiveCallback(
        //     [this](ReqPtr req) { handleStoragePortReceive(req); });
        DPRINTF(Cache,"CHIBridge Init\n");
        networkPort->setReceiveCallback(
            [this](FlitPtr &flit) { return this->handleNetworkPortReceive(flit); });
        networkPort->setCreditUnblockCallback(
            [this](Flit::CHI_CHN_TYPE channel) { handleCreditUnblock(channel); });
        networkPort->setOwner(this);
    }

    // CHIBridge::CHIBridge(const Params &p,NodeID id,SystemAddressMap* sam)
    //     : ClockedObject(p),
    //       networkPort(p,this,this->name()+"_networkPort",4),
    //       _NodeID(id),
    //       SAM(sam),
    //       TXN_Manager(1024),// default max outstanding transactions
    //       req_handle_event([this] { if (ReceiveReq(Req_tobesent.front())){Req_tobesent.pop();}}, name()),
    //         ack_handle_event([this] { TrySendCompACK();}, name())

    // {
    //     // 初始化存储端口和网络端口
    //     // storagePort->setReceiveCallback(
    //     //     [this](ReqPtr req) { handleStoragePortReceive(req); });
    //     networkPort.setReceiveCallback(
    //         [this](FlitPtr &flit) { return this->handleNetworkPortReceive(flit); });
    // }

    // CHIBridge::~CHIBridge() = default;

    bool CHIBridge::ReceiveReq(ReqPtr req, bool isRetry)
    {
        // if(req->getOpcode() == CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL) {
        //     // 对于写回请求，可能需要额外处理
        //     // 例如，可能需要等待数据传输完成
        //     kill(getpid(), SIGTRAP);
        // }
        // we assume only get requests or snoop responses from cache wrapper
        assert(req->isRequest());
        const CHI_OP_TYPE op = req->getOpcode();
        const uint64_t addr = req->getAddr();

        if ((isReadReqOp(op) || isWriteReqOp(op)) &&
            (hasInProgressWrite(addr) ||
             hasInProgressRead(addr) ||
             (!isRetry && hasQueuedReadWriteReq(addr)))) {
            enqueueBlockedReq(req);
            return true;
        }

        bool success = false;
        int txn_id = TXN_Manager.getID();
        DPRINTF(CHIBridge,
                "RecvCHIReq, op:%s, addr: %#lx, size:%u, try allocate Txn_id:%d\n",
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(req->getOpcode()),
                req->getAddr(), req->getSize(), txn_id);
        if (txn_id < 0) {
            if (!isRetry) {
                Req_tobesent.push(req); // 将请求放入待发送队列, try later //err:push req fault
            }
            DPRINTF(CHIBridge,"Txn allocate Failed,  add Req to queue, size:%d\n",Req_tobesent.size());
        }else{
            FlitPtr flit = createRequestFlit(req);
            if (!flit) {
                assert(false); // 创建失败
            }
            flit->setTxnId(txn_id);
            const CHI_OP_TYPE txOp = flit->getOpcode();
            // 尝试发送到网络端口
            DPRINTF(CHIBridge,"Try send Flit op:%s, addr: %lx, size:%d\n",CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()),flit->getAddr(),flit->getSize());
            if (networkPort->send(flit)){
                //send success, we need to save the request and txn_id
                req->setTransactionId(txn_id);
                saveOutstandingRequest(req, txn_id);
                if (isReadReqOp(op)) {
                    trackReadStart(addr);
                } else if (isWriteReqOp(op)) {
                    trackWriteStart(addr);
                }
                recordProtocolTx(txOp);
                success = true;
                DPRINTF(CHIBridge,"Send success, outstanding Req num: %d\n",outstanding_requests.size());
            }else{
                //send failed, we need to save the request and retry later
                if (flit!=nullptr) {
                    flit.reset();
                }
                TXN_Manager.releaseID(txn_id);
                if (!isRetry) {
                    Req_tobesent.push(req); // 将请求放入待发送队列
                }
                DPRINTF(CHIBridge,"Send Failed, release TxnId: %d, add Req to queue\n",txn_id);
            }
        }
        // 如果有待发送的请求，调度处理事件
        if (!Req_tobesent.empty() && !req_handle_event.scheduled()) {
            DPRINTF(CHIBridge,"Req_tobesent's number : %d ,Schedule handle event to next Cycle, tick:%d\n",Req_tobesent.size(),curTick()+clockPeriod());
            scheduleReqRetry();
        }
        return success; // 返回是否成功发送请求

    }
    void CHIBridge::ReceiveSnoopResponse(ReqPtr req)
    {
        // 处理来自wrapper的Snoop响应
        // 目前假设只处理Snoop响应
        //todo
        assert(req->isSnoopResponse());

    }

    bool CHIBridge::handleNetworkPortReceive(FlitPtr &flit)
    {
        DPRINTF(CHIBridge,"Recv CHIFlit, op:%s, srcId:%d,tgtId:%d, txn_id:%d\n",CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()),flit->getSrcId(),flit->getTgtId(),flit->getTxnId());
        recordProtocolRx(flit->getOpcode());
        switch (flit->get_Flit_Channel_Type()) {
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ: {
                // RN不应该收到 Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ 类型的Flit
                assert(false && "Received a request Flit on network port, which should not happen in CHIBridge");
                return false;
            }
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP: {
                // 处理响应Flit
                // this response Flit must be a response to a request we sent out.
                assert(TXN_Manager.isUsed(flit->getTxnId()));
                switch (outstanding_requests[flit->getTxnId()]->getOpcode()) {
                    case CHI_OP_TYPE::CHI_REQ_READUNIQUE:
                    case CHI_OP_TYPE::CHI_REQ_READSHARED:
                    case CHI_OP_TYPE::CHI_REQ_READCLEAN:{
                        // 处理读取请求的响应
                        return handleFlit_AllocatingRead(flit);
                    }
                    case CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL:
                    case CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL:{

                        return handleFlit_CopybackWrite(flit);
                    }
                    case CHI_OP_TYPE::CHI_REQ_EVICT:{
                        // 处理驱逐请求的响应
                        return handleFlit_EVICT(flit);
                    }
                    case CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE:{
                        // 处理清理唯一性请求的响应
                        return handleFlit_CLEANUNIQUE_MAKEUNIQUE(flit);
                    }
                    default: {
                        assert(false && "Not supported yet");
                        return false; // 处理失败
                    }
                }
            }
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP: {
                // 处理Snoop Flit，转化为snoopREQ发给wrapper
                // ...处理逻辑待补充...
                assert(false && "Snoop Req handling not implemented yet");
                return true; // 处理成功
            }
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA: {
                // 处理数据Flit
                // this response Flit must be a response to a request we sent out.
                assert(TXN_Manager.isUsed(flit->getTxnId()));
                switch (outstanding_requests[flit->getTxnId()]->getOpcode()) {
                    case CHI_OP_TYPE::CHI_REQ_READUNIQUE:
                    case CHI_OP_TYPE::CHI_REQ_READSHARED:
                    case CHI_OP_TYPE::CHI_REQ_READCLEAN:{
                        // 处理读取请求的响应
                        return handleFlit_AllocatingRead(flit);
                    }
                    default: {
                        assert(false && "Not supported yet");
                        return false; // 处理失败
                    }
                }
            }
            default:
                assert(false && "Unknown Flit type");
                return false; // 处理失败
        }
    }



    FlitPtr CHIBridge::createRequestFlit(ReqPtr req)
    {
        // 创建请求Flit
        FlitPtr flit = std::make_unique<Flit>(req->getOpcode(),req->getAddr(),req->getSize());
        if (!flit) {
            return nullptr; // 创建失败
        }

        // 设置Flit的相关字段
        flit->setOpcode(req->getOpcode());
        uint64_t addr = req->getAddr();
        uint32_t tgtID = SAM->getTargetID(addr);
        flit->setTgtId(tgtID);
        flit->setSrcId(_NodeID);
        flit->setCacheResponding(req->getCacheResponding());
        flit->setResponderHadWritable(req->getResponderHadWritable());
        DPRINTF(CHIBridge,
                "Create Flit, op:%s, addr: %#lx, size:%u, tgtId:%u, SrcId:%u\n",
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(req->getOpcode()),
                req->getAddr(), req->getSize(), SAM->getTargetID(req->getAddr()),
                _NodeID);
        // flit->setTxnId(GenTxnID(flit));


        // 设置其他必要的字段
        // ...设置逻辑待补充...

        return flit;
    }

    FlitPtr CHIBridge::createResponseFlit()
    {
        // ...实际实现待补充...
        return nullptr;
    }

    FlitPtr CHIBridge::createSnoopFlit()
    {
        // ...实际实现待补充...
        return nullptr;
    }

    FlitPtr CHIBridge::createDataFlit()
    {
        // ...实际实现待补充...
        return nullptr;
    }
    void CHIBridge::saveOutstandingRequest(ReqPtr &req, uint32_t txn_id)
    {
        // 保存未完成的请求
        //make sure the txn_id is not used by previous request
         assert(outstanding_requests.count(txn_id) == 0 &&
                "TxnID already used by another request");
        outstanding_requests[txn_id] = req;
    }
    bool CHIBridge::handleFlit_AllocatingRead(FlitPtr &flit)
    {
        ReqPtr req = outstanding_requests[flit->getTxnId()];
        assert(req && "Request not found for the given TxnID");
        switch (flit->getOpcode()) {
            case CHI_OP_TYPE::CHI_DAT_COMPDATA:{
                // if we receive a CompData Flit,
                req->gatherDataFlit(flit); // 收集数据Flit
                DPRINTF(CHIBridge,
                        "Gather data Flit, op:%s, addr: %#lx, size:%u, txn_id:%u, dataId:%u\n",
                        CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()),
                        flit->getAddr(), flit->getSize(), flit->getTxnId(),
                        flit->getDataId());

                if (req->dataTransferFinished()) {
                    // we have received both data and response, so we can finish this request
                    FinishReq_Read(flit);
                    sendCompACK(flit);//todo : what to do if send fail?
                }
                return true;
            }
            case CHI_OP_TYPE::CHI_DAT_DATASEPRESP: {

                req->gatherDataFlit(flit); // 收集数据Flit
                DPRINTF(CHIBridge,
                        "Gather data Flit, op:%s, addr: %#lx, size:%u, txn_id:%u, dataId:%u\n",
                        CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()),
                        flit->getAddr(), flit->getSize(), flit->getTxnId(),
                        flit->getDataId());

                if (req->dataTransferFinished() && req->isRecvSepData()) {
                        // we have received both data and response, so we can finish this request
                        FinishReq_Read(flit);
                }
                return true;
            }
            case CHI_OP_TYPE::CHI_RSP_RESPSEPDATA: {
                // 如果收到 CHI_RSP_RESPSEPDATA Flit，表示接收分离数据
                req->setRecvSepData(); // 标记为接收分离数据
                if (req->dataTransferFinished()) {
                    // we have received both data and response, so we can finish this request
                    FinishReq_Read(flit);
                }
                // even if we dont have all data, we still send a CompACK Flit to storage
                sendCompACK(flit);//todo : what to do if send fail?
                return true;
            }
            default:
                panic("Unsupported read opcode %s in AllocatingRead txn=%u",
                      CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()),
                      flit->getTxnId());
                return false;

        }
    }
    void CHIBridge::FinishReq_Read(FlitPtr &flit)
    {
        // 处理读取请求完成的逻辑
        ReqPtr req = outstanding_requests[flit->getTxnId()];
        assert(req && "Request not found for the given TxnID");

        // todo : construct a response REQ to storage
        ReqPtr response_req = req->createReadResponse();
        assert(response_req && "Failed to create response request");
        if (req->getCacheResponding()) {
            assert(req->getOpcode() == CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE);
            // this is a upgradereq that dont need cache resp, because its set CacheResponding;
        }else if (req->getOpcode() != CHI_OP_TYPE::CHI_REQ_EVICT){
            recvReadResp_callback(response_req); // 发送响应请求到存储端口
        }

        // 发送完成请求的逻辑
        TXN_Manager.releaseID(flit->getTxnId());
        trackReadFinish(req->getAddr());
        if (txnComplete_callback) {
            txnComplete_callback(req);
        }
        outstanding_requests.erase(flit->getTxnId());
        DPRINTF(CHIBridge,"Finish read request: txn_id=%d, outstanding_requests.size()=%d\n", flit->getTxnId(), outstanding_requests.size());
    }

    void CHIBridge::sendCompACK(FlitPtr &flit)
    {
        // 发送COMPACK Flit到网络
        FlitPtr compack_flit = std::make_unique<Flit>();
        assert(compack_flit && "Failed to create Compack Flit");
        compack_flit->setOpcode(CHI_OP_TYPE::CHI_RSP_COMPACK);
        switch (flit->getOpcode()) {
            case CHI_OP_TYPE::CHI_DAT_COMPDATA:{
                compack_flit->setTgtId(flit->getHomeNid());
                compack_flit->setSrcId(_NodeID);
                compack_flit->setTxnId(flit->getDbid());
                break;
            }
            case CHI_OP_TYPE::CHI_RSP_RESPSEPDATA:
            case CHI_OP_TYPE::CHI_RSP_COMP:{
                compack_flit->setTgtId(flit->getSrcId());
                compack_flit->setSrcId(_NodeID);
                compack_flit->setTxnId(flit->getDbid());
                break;
            }
            default:
                assert(false && "illegal opcode for COMPACK Flit");
                break; // 不支持的操作码
        }
        Ack_tobesent.push(std::move(compack_flit)); // 将COMPACK Flit放入待发送队列
        ack_enqueue_ticks.push(curTick());
        const size_t pending = Ack_tobesent.size();
        if (pending > maxCompAckPending) {
            maxCompAckPending = pending;
            stats.wait_compack_pending_max = pending;
        }
        TrySendCompACK();
    }
    void CHIBridge::TrySendCompACK()
    {
        DPRINTF(CHIBridge,"TrySendCompACK called, Ack_tobesent's number : %d, txn_id: %d\n",Ack_tobesent.size(), Ack_tobesent.front() ? Ack_tobesent.front()->getTxnId() : -1);
        assert(!Ack_tobesent.empty() && "Ack_tobesent should not be empty when TrySendCompACK is called");
        assert(Ack_tobesent.size() == ack_enqueue_ticks.size());
        if (networkPort->send(Ack_tobesent.front())){
            const Tick enqueueTick = ack_enqueue_ticks.front();
            Counter waitCycles = 0;
            if (curTick() > enqueueTick) {
                waitCycles = (curTick() - enqueueTick) / clockPeriod();
            }
            stats.wait_compack_cycles += waitCycles;
            stats.wait_compack_cycles_hist.sample(waitCycles);
            stats.wait_compack_sent_total++;
            recordProtocolTx(CHI_OP_TYPE::CHI_RSP_COMPACK);
            assert(Ack_tobesent.front()==nullptr);
            DPRINTF(CHIBridge,"Send CompACK Flit success.\n");
            Ack_tobesent.pop(); // 发送成功，移除已发送的COMPACK Flit
            ack_enqueue_ticks.pop();
        }else{
            assert(Ack_tobesent.front()!=nullptr);
            DPRINTF(CHIBridge,"Send CompACK Flit failed, will retry later.\n");
        }
        // 尝试发送COMPACK Flit
        if (!Ack_tobesent.empty() && !ack_handle_event.scheduled()) {
            scheduleCompAckRetry();
        }
    }

    void
    CHIBridge::scheduleReqRetry()
    {
        if (Req_tobesent.empty() || req_handle_event.scheduled()) {
            return;
        }
        if (networkPort->isChannelBlockedByCredit(
                Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ)) {
            return;
        }
        schedule(req_handle_event, curTick() + clockPeriod());
    }

    void
    CHIBridge::scheduleCompAckRetry()
    {
        if (Ack_tobesent.empty() || ack_handle_event.scheduled()) {
            return;
        }
        if (networkPort->isChannelBlockedByCredit(
                Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP)) {
            return;
        }
        schedule(ack_handle_event, curTick() + clockPeriod());
    }

    void
    CHIBridge::handleCreditUnblock(Flit::CHI_CHN_TYPE channel)
    {
        if (channel == Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ) {
            if (!Req_tobesent.empty()) {
                scheduleReqRetry();
            }
            return;
        }

        if (channel == Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP) {
            if (!Ack_tobesent.empty()) {
                scheduleCompAckRetry();
            }
        }
    }

    bool CHIBridge::handleFlit_CopybackWrite(FlitPtr &flit)
    {
        ReqPtr req = outstanding_requests[flit->getTxnId()];
        assert(req && "Request not found for the given TxnID");
        // 处理分配写入的Flit
        switch (flit->getOpcode()) {
            case CHI_OP_TYPE::CHI_RSP_COMPDBIDRESP:{
                //once we recv a CompDBIDResp Flit, we should start data flit sending.
                //todo : construct data Flits and send them out, after that we can finish the request
                uint32_t data_id = req->generateWriteDataID();
                FlitPtr data_flit = std::make_unique<Flit>();
                assert(data_flit && "Failed to create data Flit");
                data_flit->setOpcode(CHI_OP_TYPE::CHI_DAT_COPYBACKWRDATA);
                data_flit->setDataId(data_id);
                data_flit->setCcid(0); // assuming CCID is always 0
                data_flit->setSize(req->getSize());
                data_flit->setData(req);
                data_flit->setTgtId(flit->getSrcId());
                data_flit->setSrcId(_NodeID);
                data_flit->setTxnId(flit->getDbid());
                const CHI_OP_TYPE txOp = data_flit->getOpcode();

                if (networkPort->send(data_flit)){
                    //send success, we can save the request and txn_id
                    recordProtocolTx(txOp);
                    req->finishTransferdata(data_id);
                }else{
                    //send failed, we cannot delete flit from buffer
                    return false;
                }
                if (req->dataTransferFinished()){
                    // 发送完成请求的逻辑
                    TXN_Manager.releaseID(flit->getTxnId());
                    trackWriteFinish(req->getAddr());
                    if (txnComplete_callback) {
                        txnComplete_callback(req);
                    }
                    outstanding_requests.erase(flit->getTxnId());
                    DPRINTF(CHIBridge, "Finish write request: txn_id=%d, outstanding_requests.size()=%d\n", flit->getTxnId(), outstanding_requests.size());
                    return true;
                }
                return false; // 还没有完成数据传输，不能返回true
            }
            default:
                assert(false && "Unsupported write opcode");
                return false;

        }
        return false;
    }
    bool CHIBridge::handleFlit_EVICT(FlitPtr &flit)
    {
        // 处理驱逐请求的Flit
        switch (flit->getOpcode()) {
            case CHI_OP_TYPE::CHI_RSP_COMP:{
                //do finish req stuff
                // 发送完成请求的逻辑
                // treat this type of request as a read request,
                // so we can reuse the FinishReq_Read function
                FinishReq_Read(flit);
                DPRINTF(CHIBridge, "Finish evict request: txn_id=%d, outstanding_requests.size()=%d\n", flit->getTxnId(), outstanding_requests.size());
                return true;
            }
            default:
                assert(false && "Unsupported EVICT opcode");
                return false;
        }
    }
    bool CHIBridge::handleFlit_CLEANUNIQUE_MAKEUNIQUE(FlitPtr &flit)
    {
        // 处理清理唯一性请求的Flit
        switch (flit->getOpcode()) {
            case CHI_OP_TYPE::CHI_RSP_COMP:{
                // sendback a response to storage,maybe we can directly call L2wrapper's function?

                //do finish req stuff,and send ack to HN
                sendCompACK(flit);

                // 发送完成请求的逻辑
                // treat this type of request as a read request,
                // so we can reuse the FinishReq_Read function
                FinishReq_Read(flit);

                return true;
            }
            default:
                assert(false && "Unsupported CLEANUNIQUE_MAKEUNIQUE opcode");
                return false;
        }
    }
    void
    CHIBridge::init(){
        return;
    }

    bool
    CHIBridge::isSnpOpcode(CHI_OP_TYPE op)
    {
        return op > CHI_OP_TYPE::CHI_SNP_OP_START &&
               op < CHI_OP_TYPE::CHI_SNP_OP_END;
    }

    bool
    CHIBridge::isWriteEvictOpcode(CHI_OP_TYPE op)
    {
        return op == CHI_OP_TYPE::CHI_REQ_WRITEEVICTFULL ||
               op == CHI_OP_TYPE::CHI_REQ_WRITEEVICTOREVICT;
    }

    size_t
    CHIBridge::opcodeToIndex(CHI_OP_TYPE op)
    {
        return static_cast<size_t>(op);
    }

    void
    CHIBridge::updateProtocolAliases(CHI_OP_TYPE op)
    {
        if (op == CHI_OP_TYPE::CHI_REQ_READSHARED) {
            stats.protocol_readshared_total++;
        }
        if (isWriteEvictOpcode(op)) {
            stats.protocol_writeevict_total++;
        }
        if (op == CHI_OP_TYPE::CHI_RSP_COMPACK) {
            stats.protocol_compack_total++;
        }
        if (isSnpOpcode(op)) {
            stats.protocol_snp_total++;
        }
    }

    void
    CHIBridge::recordProtocolTx(CHI_OP_TYPE op)
    {
        const size_t idx = opcodeToIndex(op);
        if (idx < NumChiOps) {
            stats.protocol_tx_by_opcode[idx]++;
        }
        updateProtocolAliases(op);
    }

    void
    CHIBridge::recordProtocolRx(CHI_OP_TYPE op)
    {
        const size_t idx = opcodeToIndex(op);
        if (idx < NumChiOps) {
            stats.protocol_rx_by_opcode[idx]++;
        }
        updateProtocolAliases(op);
    }

}
}
