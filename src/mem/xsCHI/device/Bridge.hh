#pragma once
#include "../base/Network/NodeID.hh"
#include "../base/Network/SystemAddressMap.hh"
#include "../base/flit.hh"
#include "../base/module.hh"
#include "../base/port.hh"
#include "../base/request.hh"

namespace gem5
{
namespace xsCHI
{


class Bridge : public Module
{

    private:
        Port<ReqPtr> *storagePort; // 存储端口
        Port<FlitPtr> *networkPort;     // 网络端口
        NodeID _NodeID; // 节点ID,gain from it's owner module
        SystemAddressMap *SAM; // 系统地址映射，用于生成目标ID
    public:
        Bridge();
        ~Bridge();
        Port<ReqPtr>* getStoragePort() const { return storagePort; }
        Port<FlitPtr>* getNetworkPort() const { return networkPort; }
        bool handleStoragePortReceive(ReqPtr &req);
        bool handleNetworkPortReceive(FlitPtr &flit);
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

        class TxnIDManager
        {
        public:
            TxnIDManager() :  next_id(0) {
                used_ids.reset();
            }

            // 获取一个可用的TxnID，返回-1表示无可用ID
            int getID() {
                for (int i = 0; i < max_outstanding; ++i) {
                    int candidate = (next_id + i) % max_ids;
                    if (!used_ids.test(candidate)) {
                        used_ids.set(candidate);
                        next_id = (candidate + 1) % max_ids;
                        return candidate;
                    }
                }
                return -1; // 没有可用ID
            }

            // 释放一个TxnID（收到所有响应或RetryAck后调用）
            void releaseID(int id) {
                if (id >= 0 && id < max_ids)
                    used_ids.reset(id);
            }

            // 判断ID是否被占用
            bool isUsed(int id) const {
                if (id >= 0 && id < max_ids)
                    return used_ids.test(id);
                return false;
            }

        private:
            static constexpr int max_ids = 4096; // 12-bit
            static constexpr int max_outstanding = 1024;
            std::bitset<max_ids> used_ids;
            int next_id;
        };

        TxnIDManager TXN_Manager; // 事务ID管理器

        FlitPtr createRequestFlit();
        FlitPtr createResponseFlit();
        FlitPtr createSnoopFlit();
        FlitPtr createDataFlit();


        //link credit module

        //return whether the link can send out flit, considering the condition of link credit.
        bool LinkCanOut();



};

// class L2Warper : public Module
// {
//     // ...L2Warper特有成员...
// };
}
}
