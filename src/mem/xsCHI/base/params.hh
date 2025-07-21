#pragma once
namespace gem5{
namespace xsCHI {
    const int DATA_TRANSFER_WIDTH = 256; // 数据传输宽度，单位为bit
    const int DATA_TRANSFER_WIDTH_BYTE = DATA_TRANSFER_WIDTH / 8; // 数据传输宽度，单位为byte
    const int CACHEBLOCK_SIZE = 64; // 缓存行大小，单位为byte
    const int DATAFLITS_PER_TRANSACTION =  CACHEBLOCK_SIZE / DATA_TRANSFER_WIDTH_BYTE; // 每个事务的数据Flit数量
    const int RN_CHANNEL_BUFFER_SIZE = 4; // RN通道缓冲区大小
    const int XP_CHANNEL_BUFFER_SIZE = 4; // XP通道缓冲区大小
    const int HN_POCQ_SIZE = 32; // HN POCQ大小
    const int PortTransferLatency = 1; // 端口传输延迟，单位为时钟周期
}
}