#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../base/Network/NodeID.hh"

// #include <boost/dynamic_bitset.hpp>
// #include <vector>
// #include "base/types.hh"
// #include "cpu/pred/btb/stream_struct.hh"
using namespace gem5::xsCHI;

// 合法样例测试：遍历所有合法的 x 和 y 组合
TEST(NodeIDTest, ValidCases) {
    int mesh_bits = 5;

    for (uint32_t x = 0; x < (1u << mesh_bits); ++x) {
        for (uint32_t y = 0; y < (1u << mesh_bits); ++y) {
            for (uint32_t port = 0; port <= 1; ++port) {
                NodeID nid(x, y, port);
                uint32_t node_id_val = nid.getNodeID();
                NodeID nid2 = nid.createFromNodeID(node_id_val);
                EXPECT_EQ(nid.getXCoord(), nid2.getXCoord());
                EXPECT_EQ(nid.getYCoord(), nid2.getYCoord());
                EXPECT_EQ(nid.getPort(), nid2.getPort());
                EXPECT_TRUE(nid = nid2);
            }
        }
    }

}

// 非法样例测试
TEST(NodeIDTest, InvalidPort) {
    int mesh_bits = 4;
    uint32_t x = 1, y = 2;
    // port非法，断言失败
    EXPECT_DEATH(NodeID(x, y, 2), ".*");
}


TEST(NodeIDTest, InvalidMeshBitsHigh) {
    uint32_t x = 99, y = 127;

    EXPECT_DEATH(NodeID(x, y, 1), ".*");
    EXPECT_DEATH(NodeID(x, y, 1), ".*");
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
