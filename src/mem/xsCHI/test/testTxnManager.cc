#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "mem/xsCHI/base/Network/TxnManager.hh"

// #include <boost/dynamic_bitset.hpp>
// #include <vector>
// #include "base/types.hh"
// #include "cpu/pred/btb/stream_struct.hh"
using namespace gem5::xsCHI;

// 合法样例测试：遍历所有合法的 x 和 y 组合
TEST(TxnIDManagerTest, ValidCases) {
    TxnIDManager test(1024);
    for (int i = 0;i<1024;i++){
        EXPECT_EQ(i,test.getID());
    }
    EXPECT_EQ(-1,test.getID());
    for (int i = 1024;i<4096;i++){
        test.releaseID(i-512);
        EXPECT_EQ(i,test.getID());
    }
    EXPECT_EQ(-1,test.getID());
    test.releaseID(4095);
    EXPECT_EQ(512,test.getID());

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
