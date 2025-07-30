#pragma once
#include <cassert>
#include <map>
#include <memory>
#include <unordered_map>

#include "mem/xsCHI/base/CHIPort.hh"
#include "mem/xsCHI/base/Network/NodeID.hh"
#include "mem/xsCHI/base/Network/SystemAddressMap.hh"
#include "mem/xsCHI/base/Network/TxnManager.hh"
#include "mem/xsCHI/base/flit.hh"
#include "mem/xsCHI/base/module.hh"
#include "mem/xsCHI/base/request.hh"
#include "params/BIPRP.hh"
#include "params/CHIBridge.hh"
#include "params/L2ToDramSys.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace xsCHI
{
// class BridgeParams : public ClockedObjectParams
// {
//   public:
//     int buffsize; // 新增参数
//     // ...
// };

class CHIBridge : public ClockedObject
{

    private:
        // CHIPort<ReqPtr> *storagePort; // 存储端口
        CHIPort* networkPort;     // 网络端口
        NodeID _NodeID; // 节点ID,gain from it's owner module
        std::shared_ptr<SystemAddressMapRN> SAM; // 系统地址映射，用于生成目标ID
    public:
        typedef CHIBridgeParams Params;
        CHIBridge(const Params &p);
        // CHIBridge(const Params &p,NodeID id,SystemAddressMap* sam);
        CHIBridge();
        // ~CHIBridge();
        // Port<ReqPtr>* getStoragePort() const { return storagePort; }
        CHIPort* getNetworkPort()  { return networkPort; }
        bool ReceiveReq(ReqPtr req);
        void ReceiveSnoopResponse(ReqPtr req);
        bool handleNetworkPortReceive(FlitPtr &flit);

        // void initState() override;
        void init() override;
        void setNodeID(NodeID id){_NodeID = *std::make_shared<NodeID>(id);}
        void setSAM(std::shared_ptr<SystemAddressMapRN> sam){SAM = sam;}
    private:
        //All CHI transactions require a target ID to route packets from source to destination. For
        //addressable requests, a System Address Map (SAM) determines the target ID. Each node that can
        //generate a CHI addressable request contains a SAM
        uint32_t GenTarID(FlitPtr &flit);

        uint32_t GenSrcID(FlitPtr &flit);
        uint32_t GenTxnID(FlitPtr &flit);
        uint32_t GenHomeNID(FlitPtr &flit);
        uint32_t GenReturnNID(FlitPtr &flit);
        uint32_t GenReturnTxnID(FlitPtr &flit);
        uint32_t GenFwdNid(FlitPtr &flit);
        uint32_t GenLpid(FlitPtr &flit);
        uint32_t GenPgroupID(FlitPtr &flit);
        uint32_t GenStashNid(FlitPtr &flit);

        TxnIDManager TXN_Manager; // 事务ID管理器

        std::unordered_map<int, ReqPtr> outstanding_requests; // 存储由本节点产生的、未完成的请求

        void saveOutstandingRequest(ReqPtr &req, uint32_t txn_id);

        FlitPtr createRequestFlit(ReqPtr req);
        FlitPtr createResponseFlit();
        FlitPtr createSnoopFlit();
        FlitPtr createDataFlit();


        //link credit module


        // 由本节点创建的transaction完成时调用，构造一个response发送到wrapper，还需要注销TxnID，storage侧不需要考虑时延和流控。
        void finishTxnAndSendReq(ReqPtr &req);

        // 某一个transaction在处理消息中决定发送一个Flit时调用，将这个Flit加入仲裁逻辑中进行发送。
        void sendFlit(FlitPtr &flit);

        //readclean readunique readshared readnotshareddirty readprefferunique
        bool handleFlit_AllocatingRead(FlitPtr &flit);

        //writebackfull writecleanfull writebackptl writeevictfull
        bool handleFlit_CopybackWrite(FlitPtr &flit);

        bool handleFlit_CLEANUNIQUE_MAKEUNIQUE(FlitPtr &flit);//cleanunique,makeunique
        bool handleFlit_EVICT(FlitPtr &flit);//evict

        void FinishReq_Read(FlitPtr &flit);
        void sendCompACK(FlitPtr &flit);
        void TrySendCompACK();

        void TrySendReq();

        std::queue<ReqPtr> Req_tobesent; // 用于存储待发送的请求Flit,无限长
        EventFunctionWrapper req_handle_event; // 用于处理请求发送的事件
        std::queue<FlitPtr> Ack_tobesent; // 用于存储待发送的ACK Flit,无限长
        EventFunctionWrapper ack_handle_event; // 用于处理ACK发送的事件

        std::function<void(ReqPtr&)> recvReadResp_callback;//callback from L2Wrapper to handle read response
    public:
        void set_recvReadResp_callback(std::function<void(ReqPtr&)> callback) {recvReadResp_callback = callback;};
};

// class L2Warper : public Module
// {
//     // ...L2Warper特有成员...
// };
}
}
