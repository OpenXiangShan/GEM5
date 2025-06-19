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
            [this](ReqPtr &req) { handleStoragePortReceive(req); });
        networkPort->setReceiveCallback(
            [this](FlitPtr &flit) { handleNetworkPortReceive(flit); });
    }

    Bridge::~Bridge() = default;

    bool Bridge::handleStoragePortReceive(ReqPtr &req)
    {
        if (!LinkCanOut()) {
            return false; // 如果链路不可用，直接返回
        }
        return true;
    }

    bool Bridge::handleNetworkPortReceive(FlitPtr &flit)
    {
        // ...实际处理逻辑待实现...
        return true;
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

    FlitPtr Bridge::createRequestFlit()
    {
        // ...实际实现待补充...
        return nullptr;
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

}
}
