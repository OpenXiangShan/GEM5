#include <cassert>
#include <memory>

#include "Bridge.hh"

namespace gem5
{
namespace xsCHI
{
    Bridge::Bridge()
        : storagePort(new Port<ReqPtr>(this)),
          networkPort(new Port<FlitPtr>(this)),
          _NodeID(0),
          SAM(nullptr)
    {
        // 初始化存储端口和网络端口
        storagePort->setReceiveCallback(
            [this](ReqPtr req) { handleStoragePortReceive(req); });
        networkPort->setReceiveCallback(
            [this](FlitPtr flit) { handleNetworkPortReceive(flit); });
    }

    Bridge::~Bridge() = default;

    bool Bridge::handleStoragePortReceive(ReqPtr &req)
    {
        // we assume only get requests or snoop responses from cache wrapper
        assert(req->isRequest() || req->isSnoopResponse());
        if (!LinkCanOut()) {
            return false; // 如果链路不可用，直接返回
        }
        if (req->isRequest()) {
            // 处理请求
            FlitPtr flit = createRequestFlit(req);
            if (!flit) {
                return false; // 创建失败
            }
            int txn_id = TXN_Manager.getID();
            if (txn_id < 0) {
                flit.reset();
                return false; // 没有可用的TxnID
            }
            flit->setTxnId(txn_id);
            saveOutstandingRequest(req, txn_id);
            // 发送到网络端口
            networkPort->send(std::move(flit));
        } else if (req->isSnoopResponse()) {
            // this is a snoop response to a Snoop Req which we previously recv a snoop flit and created.
            // Snoop part is not implemented yet.
            assert(false && "Snoop response handling not implemented yet");
        } else {
            assert(false && "illegal request type");
        }

        return true;
    }

    bool Bridge::handleNetworkPortReceive(FlitPtr &flit)
    {
        switch (flit->get_Flit_Channel_Type()) {
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ: {
                // RN不应该收到 Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ 类型的Flit
                // ...处理逻辑待补充...
                assert(false && "Received a request Flit on network port, which should not happen in Bridge");
                return true; // 处理成功
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
                        handleFlit_AllocatingRead(flit);
                        break;
                    }
                    case CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL:
                    case CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL:{

                        handleFlit_CopybackWrite(flit);
                        break;
                    }
                    case CHI_OP_TYPE::CHI_REQ_EVICT:{
                        // 处理驱逐请求的响应
                        handleFlit_EVICT(flit);
                        break;
                    }
                    case CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE:{
                        // 处理清理唯一性请求的响应
                        handleFlit_CLEANUNIQUE_MAKEUNIQUE(flit);
                        break;
                    }
                    default: {
                        assert(false && "Not supported yet");
                        return false; // 处理失败
                    }
                }
                return true; // 处理成功
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
                        handleFlit_AllocatingRead(flit);
                        break;
                    }
                    default: {
                        assert(false && "Not supported yet");
                        return false; // 处理失败
                    }
                }
                return true; // 处理成功
            }
            default:
                assert(false && "Unknown Flit type");
                return false; // 处理失败
        }
    }

    uint32_t Bridge::GenTarID(FlitPtr &flit)
    {
        // 需要SAM和flit中的地址
        if (SAM && flit) {
            return SAM->getTargetID(flit->getAddr());
        }
        return 0;
    }

    uint32_t Bridge::GenSrcID(FlitPtr &flit)
    {
        // 通常返回本节点ID
        return _NodeID.getNodeID();
    }

    uint32_t Bridge::GenTxnID(FlitPtr &flit)
    {
        // 生成TxnID，实际实现可根据flit类型区分
        int id = TXN_Manager.getID();
        return id >= 0 ? static_cast<uint32_t>(id) : 0xFFFFFFFF;
    }

    uint32_t Bridge::GenHomeNID(FlitPtr &flit)
    {
        // ...实际实现可根据flit内容决定...
        return 0;
    }

    uint32_t Bridge::GenReturnNID(FlitPtr &flit)
    {
        // ...实际实现可根据flit内容决定...
        return 0;
    }

    uint32_t Bridge::GenReturnTxnID(FlitPtr &flit)
    {
        // ...实际实现可根据flit内容决定...
        return 0;
    }

    uint32_t Bridge::GenFwdNid(FlitPtr &flit)
    {
        // ...实际实现可根据flit内容决定...
        return 0;
    }

    uint32_t Bridge::GenLpid(FlitPtr &flit)
    {
        // ...实际实现可根据flit内容决定...
        return 0;
    }

    uint32_t Bridge::GenPgroupID(FlitPtr &flit)
    {
        // ...实际实现可根据flit内容决定...
        return 0;
    }

    uint32_t Bridge::GenStashNid(FlitPtr &flit)
    {
        // ...实际实现可根据flit内容决定...
        return 0;
    }

    FlitPtr Bridge::createRequestFlit(ReqPtr req)
    {
        // 创建请求Flit
        FlitPtr flit = std::make_unique<Flit>();
        if (!flit) {
            return nullptr; // 创建失败
        }

        // 设置Flit的相关字段
        flit->setOpcode(req->getOpcode());
        flit->setTgtId(SAM->getTargetID(req->getAddr()));
        flit->setSrcId(_NodeID.getNodeID());
        // flit->setTxnId(GenTxnID(flit));


        // 设置其他必要的字段
        // ...设置逻辑待补充...

        return flit;
    }

    FlitPtr Bridge::createResponseFlit()
    {
        // ...实际实现待补充...
        return nullptr;
    }

    FlitPtr Bridge::createSnoopFlit()
    {
        // ...实际实现待补充...
        return nullptr;
    }

    FlitPtr Bridge::createDataFlit()
    {
        // ...实际实现待补充...
        return nullptr;
    }
    void Bridge::saveOutstandingRequest(ReqPtr &req, uint32_t txn_id)
    {
        // 保存未完成的请求
        //make sure the txn_id is not used by previous request
         assert(outstanding_requests.count(txn_id) == 0 &&
                "TxnID already used by another request");
        outstanding_requests[txn_id] = req;
    }
    void Bridge::handleFlit_AllocatingRead(FlitPtr &flit)
    {
        ReqPtr req = outstanding_requests[flit->getTxnId()];
        assert(req && "Request not found for the given TxnID");
        switch (flit->getOpcode()) {
            case CHI_OP_TYPE::CHI_DAT_COMPDATA:{
                // if we receive a CompData Flit,
                req->gatherDataFlit(flit); // 收集数据Flit

                if (req->dataTransferFinished()) {
                    // we have received both data and response, so we can finish this request
                    FinishReq_Read(flit);
                    sendCompACK(flit);
                }
                return;
            }
            case CHI_OP_TYPE::CHI_DAT_DATASEPRESP: {

                req->gatherDataFlit(flit); // 收集数据Flit

                if (req->dataTransferFinished() && req->isRecvSepData()) {
                        // we have received both data and response, so we can finish this request
                        FinishReq_Read(flit);
                }
                return;
            }
            case CHI_OP_TYPE::CHI_RSP_RESPSEPDATA: {
                // 如果收到 CHI_RSP_RESPSEPDATA Flit，表示接收分离数据
                req->setRecvSepData(); // 标记为接收分离数据
                if (req->dataTransferFinished()) {
                    // we have received both data and response, so we can finish this request
                    FinishReq_Read(flit);
                }
                // even if we dont have all data, we still send a CompACK Flit to storage
                sendCompACK(flit);
                return;
            }
            default:
                assert(false && "Unsupported read opcode");
                return; // 不支持的读取操作码

        }
    }
    void Bridge::FinishReq_Read(FlitPtr &flit)
    {
        // 处理读取请求完成的逻辑
        ReqPtr req = outstanding_requests[flit->getTxnId()];
        assert(req && "Request not found for the given TxnID");

        // todo : construct a response REQ to storage
        ReqPtr response_req = req->createReadResponse();
        assert(response_req && "Failed to create response request");
        storagePort->send(response_req); // 发送响应请求到存储端口

        // 发送完成请求的逻辑
        TXN_Manager.releaseID(flit->getTxnId());
        outstanding_requests.erase(flit->getTxnId());
    }

    void Bridge::sendCompACK(FlitPtr &flit)
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
        networkPort->send(std::move(compack_flit));
    }

    void Bridge::handleFlit_CopybackWrite(FlitPtr &flit)
    {
        // 处理分配写入的Flit
        switch (flit->getOpcode()) {
            case CHI_OP_TYPE::CHI_RSP_COMPDBIDRESP:{
                //once we recv a CompDBIDResp Flit, we should start data flit sending.
                //todo : construct data Flits and send them out, after that we can finish the request
                break;
            }
            default:
                assert(false && "Unsupported write opcode");
                break; // 不支持的读取操作码

        }
    }
    void Bridge::handleFlit_EVICT(FlitPtr &flit)
    {
        // 处理驱逐请求的Flit
        switch (flit->getOpcode()) {
            case CHI_OP_TYPE::CHI_RSP_COMP:{
                //do finish req stuff
                // 发送完成请求的逻辑
                TXN_Manager.releaseID(flit->getTxnId());
                outstanding_requests.erase(flit->getTxnId());
                break;
            }
            default:
                assert(false && "Unsupported EVICT opcode");
                break; // 不支持的驱逐操作码
        }
    }
    void Bridge::handleFlit_CLEANUNIQUE_MAKEUNIQUE(FlitPtr &flit)
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

                break;
            }
            default:
                assert(false && "Unsupported CLEANUNIQUE_MAKEUNIQUE opcode");
                break; // 不支持的清理唯一性操作码
        }
    }

}
}
