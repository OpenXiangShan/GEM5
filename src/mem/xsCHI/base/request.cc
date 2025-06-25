#include <cassert>

#include "request.hh"

namespace gem5
{
namespace xsCHI
{
    void Request::gatherDataFlit(FlitPtr &flit){
        // do some check
        assert(flit->get_Flit_Channel_Type() == Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA &&
         "gatherDataFlit should only be called for Data Flit");
        // 检查Flit的TxnID是否与当前Request的TxnID匹配
        assert(flit->getTxnId() == transaction_id &&
               "Flit TxnID does not match Request TxnID");
        if (data != nullptr) {
            assert(data == flit->getData() &&
                   "Data Flit data  does not match existing data");
        } else {
            // 如果data为空，说明这是第一次接收数据Flit
            data = flit->getData();
        }
        assert(!dataFlitsReceived.test(  flit->getDataId()) &&
               "Data Flit with this DataId has already been received");
        dataFlitsReceived.set(flit->getDataId());

    }
}}
