#pragma once
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>

#include "../base/CHIPort.hh"
#include "../base/Network/NodeID.hh"
#include "../base/Network/SystemAddressMap.hh"
#include "../base/Network/TxnManager.hh"
#include "../base/flit.hh"
#include "../base/module.hh"
#include "../base/request.hh"
#include "params/FakeL3.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace xsCHI
{
    class FakeL3 : public ClockedObject
    {
    private:
        CHIPort* networkPort;
        uint32_t _NodeID;
        std::shared_ptr<SystemAddressMapHN> SAM; // 系统地址映射，用于生成目标ID
        bool handlePortReceive(FlitPtr &flit);

        TxnIDManager TXN_Manager;
        std::unordered_map<int, ReqPtr> outstanding_requests; // 存储由本节点产生的、未完成的请求, SN only for write Transfer
        void saveOutstandingRequest(ReqPtr &req, uint32_t txn_id);
        bool handleFlit_AllocatingRead(FlitPtr &flit);
        bool handleFlit_CopybackWrite(FlitPtr &flit);
        bool handleFlit_EVICT(FlitPtr &flit);
        bool handleFlit_CLEANUNIQUE_MAKEUNIQUE(FlitPtr &flit);
    public:
        typedef FakeL3Params Params;
        FakeL3(const Params &p);
        // FakeL3(const Params &p,uint32_t id,SystemAddressMap *sam);
        FakeL3();
        // ~FakeL3() =default;
        CHIPort* getNetworkPort();
        // Compatibility helpers: FakeL3 now owns a single network port.
        CHIPort* getCHIPort_CPUSIDE() { return networkPort; }
        CHIPort* getCHIPort_MEMSIDE() { return networkPort; }
        void init() override;
        void setNodeID(uint32_t id){_NodeID = id;}
        void setSAM(std::shared_ptr<SystemAddressMapHN> sam){SAM = sam;}
        // std::string name() const override{ return "FakeL3"; }

    };
}
}
