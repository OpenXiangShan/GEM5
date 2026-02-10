#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <vector>

#include "mem/xsCHI/base/FlitOpType.hh"
#include "mem/xsCHI/base/flit.hh"
#include "mem/xsCHI/base/params.hh"
#include "mem/xsCHI/base/request.hh"

using namespace gem5::xsCHI;

static std::vector<uint8_t>
makePayload(size_t size)
{
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }
    return data;
}

TEST(P2PMainPathTest, ReadSharedMainPath)
{
    const uint64_t addr = 0x1000;
    const uint32_t size = CACHEBLOCK_SIZE;
    const uint64_t txn_id = 1;

    std::cout << "[ReadSharedMainPath] "
              << "addr=0x" << std::hex << addr
              << " size=" << std::dec << size
              << " txn_id=" << txn_id << std::endl;

    ReqPtr req = std::make_shared<Request>(
        CHI_OP_TYPE::CHI_REQ_READSHARED, addr, size);
    req->setTransactionId(txn_id);

    std::cout << "[ReadSharedMainPath] "
              << "Request created. opcode=" << static_cast<int>(req->getOpcode())
              << " dataTransferStarted=" << std::boolalpha
              << req->dataTransferStarted()
              << " dataTransferFinished=" << req->dataTransferFinished()
              << std::endl;

    auto payload = makePayload(size);
    std::cout << "[ReadSharedMainPath] Generated payload of size "
              << payload.size() << std::endl;

    for (uint16_t data_id = 0; data_id < DATAFLITS_PER_TRANSACTION; ++data_id) {
        FlitPtr dat = std::make_unique<Flit>(
            CHI_OP_TYPE::CHI_DAT_COMPDATA, addr, size);
        dat->setTxnId(txn_id);
        dat->setDataId(data_id);
        dat->setData(payload.data());
        std::cout << "[ReadSharedMainPath] Gathering data flit "
                  << "data_id=" << data_id
                  << " TxnId=" << dat->getTxnId()
                  << " size=" << dat->getSize()
                  << std::endl;
        req->gatherDataFlit(dat);
        std::cout << "[ReadSharedMainPath] "
                  << "After gather: dataTransferStarted="
                  << req->dataTransferStarted()
                  << " dataTransferFinished="
                  << req->dataTransferFinished()
                  << std::endl;
    }

    EXPECT_TRUE(req->dataTransferFinished());
    std::cout << "[ReadSharedMainPath] All data flits gathered. "
              << "dataTransferFinished=" << req->dataTransferFinished()
              << std::endl;

    ReqPtr resp = req->createReadResponse();
    std::cout << "[ReadSharedMainPath] Read response created." << std::endl;
    std::vector<uint8_t> out(size, 0);
    resp->getData(out.data());
    EXPECT_EQ(out, payload);
    std::cout << "[ReadSharedMainPath] Payload verification passed." << std::endl;
}

TEST(P2PMainPathTest, WritebackFullMainPath)
{
    const uint64_t addr = 0x2000;
    const uint32_t size = CACHEBLOCK_SIZE;

    std::cout << "[WritebackFullMainPath] "
              << "addr=0x" << std::hex << addr
              << " size=" << std::dec << size
              << std::endl;

    ReqPtr req = std::make_shared<Request>(
        CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL, addr, size);

    auto payload = makePayload(size);
    std::cout << "[WritebackFullMainPath] Generated payload of size "
              << payload.size() << std::endl;
    req->setData(payload.data());
    std::cout << "[WritebackFullMainPath] Request data set. "
              << "DataValid=" << std::boolalpha << req->DataValid()
              << std::endl;

    for (uint16_t i = 0; i < DATAFLITS_PER_TRANSACTION; ++i) {
        uint32_t data_id = req->generateWriteDataID();
        std::cout << "[WritebackFullMainPath] Iteration " << i
                  << ", generated data_id=" << data_id << std::endl;
        FlitPtr dat = std::make_unique<Flit>(
            CHI_OP_TYPE::CHI_DAT_COPYBACKWRDATA, addr, size);
        dat->setDataId(data_id);
        dat->setData(req);
        std::cout << "[WritebackFullMainPath] Created data flit "
                  << "data_id=" << dat->getDataId()
                  << " size=" << dat->getSize()
                  << std::endl;

        std::vector<uint8_t> out(size, 0);
        dat->getData(out.data());
        EXPECT_EQ(out, payload);
        std::cout << "[WritebackFullMainPath] "
                  << "Flit data matches original payload for data_id="
                  << data_id << std::endl;

        req->finishTransferdata(data_id);
        std::cout << "[WritebackFullMainPath] "
                  << "Marked data_id=" << data_id
                  << " as transferred. dataTransferFinished="
                  << req->dataTransferFinished()
                  << std::endl;
    }

    EXPECT_TRUE(req->dataTransferFinished());
    std::cout << "[WritebackFullMainPath] All data flits transferred. "
              << "dataTransferFinished=" << req->dataTransferFinished()
              << std::endl;
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}