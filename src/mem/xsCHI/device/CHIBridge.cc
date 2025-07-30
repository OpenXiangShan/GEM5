#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "CHIBridge.hh"
#include "base/trace.hh"
#include "debug/CHIBridge.hh"
#include "debug/Cache.hh"

namespace gem5
{
namespace xsCHI
{
    CHIBridge::CHIBridge(const Params &p)
        : ClockedObject(p),
          networkPort(p.networkPort),
          _NodeID(0,0,0),
          SAM(nullptr),
          TXN_Manager(1024),// default max outstanding transactions
          req_handle_event([this] { 
            DPRINTF(CHIBridge,"retry called ,Req_tobesent's number : %d reqOP:%s,addr:%lx\n",Req_tobesent.size(),CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(Req_tobesent.front()->getOpcode()),Req_tobesent.front()->getAddr());
            if (ReceiveReq(Req_tobesent.front())){
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
            ack_handle_event([this] { TrySendCompACK();}, name())

    {
        // 初始化存储端口和网络端口
        // storagePort->setReceiveCallback(
        //     [this](ReqPtr req) { handleStoragePortReceive(req); });
        DPRINTF(Cache,"CHIBridge Init\n");
        networkPort->setReceiveCallback(
            [this](FlitPtr &flit) { return this->handleNetworkPortReceive(flit); });
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

    bool CHIBridge::ReceiveReq(ReqPtr req)
    {
        // we assume only get requests or snoop responses from cache wrapper
        assert(req->isRequest());
        bool success = false;
        int txn_id = TXN_Manager.getID();
        DPRINTF(CHIBridge,"RecvCHIReq, op:%s, addr: %#x, size:%d , try allocate Txn_id:%d\n",static_cast<int>(req->getOpcode()),req->getAddr(),req->getSize(),txn_id);
        if (txn_id < 0) {
            Req_tobesent.push(req); // 将请求放入待发送队列, try later //err:push req fault
            DPRINTF(CHIBridge,"Txn allocate Failed,  add Req to queue, size:%d\n",Req_tobesent.size());
        }else{
            FlitPtr flit = createRequestFlit(req);
            if (!flit) {
                assert(false); // 创建失败
            }
            flit->setTxnId(txn_id);
            // 尝试发送到网络端口
            DPRINTF(CHIBridge,"Try send Flit op:%s, addr: %lx, size:%d\n",CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()),flit->getAddr(),flit->getSize());
            if (networkPort->send(flit)){
                //send success, we need to save the request and txn_id
                req->setTransactionId(txn_id);
                saveOutstandingRequest(req, txn_id);
                success = true;
                DPRINTF(CHIBridge,"Send success, outstanding Req num: %d\n",outstanding_requests.size());
            }else{
                //send failed, we need to save the request and retry later
                if (flit!=nullptr) {
                    flit.reset();
                }
                TXN_Manager.releaseID(txn_id);
                Req_tobesent.push(req); // 将请求放入待发送队列
                DPRINTF(CHIBridge,"Send Failed, release TxnId: %d, add Req to queue\n",txn_id);
            }
        }
        // 如果有待发送的请求，调度处理事件
        if (!Req_tobesent.empty() && !req_handle_event.scheduled()) {
            DPRINTF(CHIBridge,"Req_tobesent's number : %d ,Schedule handle event to next Cycle, tick:%d\n",Req_tobesent.size(),curTick()+clockPeriod());
            schedule(req_handle_event, curTick()+clockPeriod());
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
        flit->setSrcId(_NodeID.getNodeID());
        DPRINTF(CHIBridge,"Create Flit, op:%s, addr: %#x, size:%d , tgtId:%d ,SrcId:%d\n",\
            static_cast<int>(req->getOpcode()),req->getAddr(),req->getSize(),SAM->getTargetID(req->getAddr()),_NodeID.getNodeID());
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
                DPRINTF(CHIBridge,"Gather data Flit, op:%s, addr: %#x, size:%d , txn_id:%d, dataId:%d\n",\
                    static_cast<int>(flit->getOpcode()),flit->getAddr(),flit->getSize(),flit->getTxnId(),flit->getDataId());

                if (req->dataTransferFinished()) {
                    // we have received both data and response, so we can finish this request
                    FinishReq_Read(flit);
                    sendCompACK(flit);//todo : what to do if send fail?
                }
                return true;
            }
            case CHI_OP_TYPE::CHI_DAT_DATASEPRESP: {

                req->gatherDataFlit(flit); // 收集数据Flit
                DPRINTF(CHIBridge,"Gather data Flit, op:%s, addr: %#x, size:%d , txn_id:%d, dataId:%d\n",\
                    static_cast<int>(flit->getOpcode()),flit->getAddr(),flit->getSize(),flit->getTxnId(),flit->getDataId());

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
                assert(false && "Unsupported read opcode");
                return false; // 不支持的读取操作码

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
        recvReadResp_callback(response_req); // 发送响应请求到存储端口

        // 发送完成请求的逻辑
        TXN_Manager.releaseID(flit->getTxnId());
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
                compack_flit->setSrcId(_NodeID.getNodeID());
                compack_flit->setTxnId(flit->getDbid());
                break;
            }
            case CHI_OP_TYPE::CHI_RSP_RESPSEPDATA:
            case CHI_OP_TYPE::CHI_RSP_COMP:{
                compack_flit->setTgtId(flit->getSrcId());
                compack_flit->setSrcId(_NodeID.getNodeID());
                compack_flit->setTxnId(flit->getDbid());
                break;
            }
            default:
                assert(false && "illegal opcode for COMPACK Flit");
                break; // 不支持的操作码
        }
        Ack_tobesent.push(std::move(compack_flit)); // 将COMPACK Flit放入待发送队列
        TrySendCompACK();
    }
    void CHIBridge::TrySendCompACK()
    {
        assert(!Ack_tobesent.empty() && "Ack_tobesent should not be empty when TrySendCompACK is called");
        if (networkPort->send(Ack_tobesent.front())){
            assert(Ack_tobesent.front()==nullptr);
            Ack_tobesent.pop(); // 发送成功，移除已发送的COMPACK Flit
        }else{
            assert(Ack_tobesent.front()!=nullptr);
        }
        // 尝试发送COMPACK Flit
        if (!Ack_tobesent.empty() && !ack_handle_event.scheduled()) {
            schedule(ack_handle_event, curTick()+clockPeriod());
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
                data_flit->setData(req);
                data_flit->setSize(req->getSize());
                data_flit->setTgtId(flit->getSrcId());
                data_flit->setSrcId(_NodeID.getNodeID());
                data_flit->setTxnId(flit->getDbid());

                if (networkPort->send(data_flit)){
                    //send success, we can save the request and txn_id
                    req->finishTransferdata(data_id);
                }else{
                    //send failed, we cannot delete flit from buffer
                    return false;
                }
                if (req->dataTransferFinished()){
                    // 发送完成请求的逻辑
                    TXN_Manager.releaseID(flit->getTxnId());
                    outstanding_requests.erase(flit->getTxnId());
                    DPRINTF(CHIBridge, "Finish write request: txn_id=%d, outstanding_requests.size()=%d\n", flit->getTxnId(), outstanding_requests.size());
                }
                return true;
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
                TXN_Manager.releaseID(flit->getTxnId());
                outstanding_requests.erase(flit->getTxnId());
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
                TXN_Manager.releaseID(flit->getTxnId());
                outstanding_requests.erase(flit->getTxnId());

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

}
}
