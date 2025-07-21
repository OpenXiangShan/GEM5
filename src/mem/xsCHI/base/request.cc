#include "mem/xsCHI/base/request.hh"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "mem/xsCHI/base/flit.hh"

namespace gem5
{
namespace xsCHI
{
    void Request::gatherDataFlit(FlitPtr &flit){
        // do some check
        assert(flit->get_Flit_Channel_Type() == Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA &&
         "gatherDataFlit should only be called for Data Flit");
        // 检查Flit的TxnID是否与当前Request的TxnID匹配
        assert(flit->getTxnId() == transaction_id && "Flit TxnID does not match Request TxnID");
        uint8_t *tmp = new uint8_t[getSize()];
        assert(getSize() == flit->getSize() &&
                   "Flit size does not match Request size");
        flit->getData(tmp);
        if (DataValid()) {
            for (int i = 0; i < getSize(); ++i) {
                assert(tmp[i] == data[i] &&
                       "Data Flit data does not match existing data");
            }

        } else {
            // 如果data为空，说明这是第一次接收数据Flit
            setData(tmp);
        }
        delete[] tmp; // 释放临时内存
        assert(!dataFlitsTransferred.test(  flit->getDataId()) &&
               "Data Flit with this DataId has already been received");
        dataFlitsTransferred.set(flit->getDataId());

    }
    uint32_t Request::generateWriteDataID() {
        // 生成一个写数据Flit
        assert(data != nullptr && "Data must be set before generating write data flit");
        assert(isRequest() && "generateWriteDataFlit should only be called for request type");
        assert(!dataTransferFinished() && "Data transfer is already finished, cannot generate new data ID");
        // walk through dataFlitsTransferred map, find the first unset bit
        // 找到第一个为0的位：取反后找第一个为1的位
        auto inverted = ~dataFlitsTransferred;
        uint32_t data_id = inverted._Find_first();
        assert(data_id < DATAFLITS_PER_TRANSACTION && "Data ID exceeds maximum limit");
        return data_id;
    }
    void Request::finishTransferdata(int data_id) {
        // 完成数据传输，设置对应的位
        assert(data_id < DATAFLITS_PER_TRANSACTION && "Data ID exceeds maximum limit");
        dataFlitsTransferred.set(data_id);
    }

    Request::Request(CHI_OP_TYPE op, uint64_t addr, uint32_t size){
        this->opcode = op;
        this->addr = addr;
        this->size = size;
        this->data = nullptr;
        dataFlitsTransferred.reset();
        this->IsRecvSepData = false;
    }
    ReqPtr
    Request::createReadResponse(){
        ReqPtr resp = std::make_shared<Request>(this->opcode,this->addr,this->size);
        uint8_t *tmp = new uint8_t[getSize()];
        this->getData(tmp);
        resp->setData(tmp);
        delete[] tmp; // 释放临时内存
        return resp;
    }
}}
