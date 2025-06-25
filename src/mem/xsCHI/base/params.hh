namespace gem5{
namespace xsCHI {
    const int DATA_TRANSFER_WIDTH = 256; // 数据传输宽度，单位为bit
    const int DATA_TRANSFER_WIDTH_BYTE = DATA_TRANSFER_WIDTH / 8; // 数据传输宽度，单位为byte
    const int CACHEBLOCK_SIZE = 64; // 缓存行大小，单位为byte
    const int DATAFLITS_PER_TRANSACTION =  CACHEBLOCK_SIZE / DATA_TRANSFER_WIDTH_BYTE; // 每个事务的数据Flit数量
}
}