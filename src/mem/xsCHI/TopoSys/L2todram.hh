#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "mem/xsCHI/device/CHI_L2.hh"
#include "mem/xsCHI/device/DDRWrapper.hh"
#include "mem/xsCHI/device/MeshNode.hh"
#include "mem/xsCHI/device/fakeL3.hh"
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
            // 来自配置层的影子桥集合（与 L2Wrapper 中的 shadowBridges 一一对应）。
            std::vector<CHIBridge*> shadowBridges;
            // 每个影子的接入点，格式 meshX.localY；长度必须与 shadowBridges 一致。
            std::vector<std::string> shadowAttachPoints;
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
