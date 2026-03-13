#pragma once
#include "mem/xsCHI/device/DDRWrapper.hh"
#include "mem/xsCHI/device/fakeL3.hh"
#include "mem/xsCHI/device/CHI_L2.hh"
#include "mem/xsCHI/device/MeshNode.hh"
#include "params/ClockedObject.hh"
#include "params/L2ToDramSys.hh"
#include "sim/sim_object.hh"

namespace gem5 {
namespace xsCHI {
    class L2ToDramSys : public ClockedObject
    {
        private:
            CHI_L2* L2wrap;
            FakeL3* L3bridge;
            DDRWrapper* Dram;
            // 2x2 mesh:
            // Mesh0(0,0) <-> Mesh1(1,0)
            //   ^               ^
            //   |               |
            // Mesh3(0,1) <-> Mesh2(1,1)
            // Endpoints:
            // RN@Mesh0.local0, HN@Mesh1.local0, DRAM@Mesh2.local0
            MeshNode* Mesh0;
            MeshNode* Mesh1;
            MeshNode* Mesh2;
            MeshNode* Mesh3;
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
