#pragma once
#include "mem/xsCHI/device/DDRWrapper.hh"
#include "mem/xsCHI/device/HNF.hh"
#include "mem/xsCHI/device/L2Wrapper.hh"
#include "mem/xsCHI/device/MeshNode.hh"
#include "params/ClockedObject.hh"
#include "params/L2ToDramSys.hh"
#include "sim/sim_object.hh"

namespace gem5 {
namespace xsCHI {
    class L2ToDramSys : public ClockedObject
    {
        private:
            L2Wrapper* L2wrap;
            FakeL3* L3bridge;
            DDRWrapper* Dram;
            // Minimal 2-node mesh chain:
            // L2 endpoint -- MeshNode0 <-> MeshNode1 -- HN endpoint
            MeshNode* Mesh0;
            MeshNode* Mesh1;
        public:
            typedef L2ToDramSysParams Params;

            L2ToDramSys(const Params &p);

            // ~L2ToDramSys() = default;
            gem5::Port &getPort(const std::string &if_name,
                PortID idx=InvalidPortID) override;
            void init() override;

    };
}
}
