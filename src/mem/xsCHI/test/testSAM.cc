#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "mem/xsCHI/base/Network/NodeID.hh"
#include "mem/xsCHI/base/Network/SystemAddressMap.hh"

// #include <boost/dynamic_bitset.hpp>
// #include <vector>
// #include "base/types.hh"
// #include "cpu/pred/btb/stream_struct.hh"
using namespace gem5::xsCHI;

// 合法样例测试：遍历所有合法的 x 和 y 组合
TEST(TxnIDManagerTest, ValidCases) {
    NodeID L2ID = NodeID(0,0,0);
    NodeID L3ID = NodeID(1,1,0);
    NodeID dramID = NodeID(2,2,0);
    std::list<uint32_t> HNs(0);
    HNs.push_back(L3ID.getNodeID());
    SystemAddressMapRN L2SAM = SystemAddressMapRN(HNs);
    std::cout<<L2SAM.getTargetID(0x80000000)<<std::endl;
    EXPECT_EQ(L2SAM.getTargetID(0x80000000),L3ID.getNodeID());

}

// // 非法样例测试
// TEST(TxnIDManagerTest, InvalidPort) {
//     int mesh_bits = 4;
//     uint32_t x = 1, y = 2;
//     // port非法，断言失败
//     EXPECT_DEATH(TxnIDManager(x, y, 2), ".*");
// }


// TEST(TxnIDManagerTest, InvalidMeshBitsHigh) {
//     uint32_t x = 99, y = 127;

//     EXPECT_DEATH(TxnIDManager(x, y, 1), ".*");
//     EXPECT_DEATH(TxnIDManager(x, y, 1), ".*");
// }

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
