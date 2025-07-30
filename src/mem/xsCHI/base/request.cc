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
    Request::Request(const Request& other):
        std::enable_shared_from_this<Request>(other) // 显式初始化基类
    {
        this->opcode = other.opcode;
        this->addr = other.addr;
        this->size = other.size;
        this->data = nullptr;
        if (other.data != nullptr) {
            this->data = new uint8_t[other.size];
            std::memcpy(this->data, other.data, other.size);
        }
        this->dataFlitsTransferred = other.dataFlitsTransferred;
        this->IsRecvSepData = other.IsRecvSepData;
        this->qos_priority = other.qos_priority;
        this->target_id = other.target_id;
        this->source_id = other.source_id;
        this->transaction_id = other.transaction_id;
        this->logicalProcessor_id = other.logicalProcessor_id;
        this->pgroup_id = other.pgroup_id;
        this->deep = other.deep;
        this->return_nid = other.return_nid;
        this->return_txnid = other.return_txnid;
        this->stash_nid = other.stash_nid;
        this->stash_nid_valid = other.stash_nid_valid;
        this->stash_lpid = other.stash_lpid;
        this->stash_lpid_valid = other.stash_lpid_valid;
        this->stash_group_id = other.stash_group_id;
        this->ccid = other.ccid;
        this->data_id = other.data_id;
        this->ns = other.ns;
        this->allow_retry = other.allow_retry;
        this->pcrd_type = other.pcrd_type;
        this->exp_comp_ack = other.exp_comp_ack;
        this->mem_attr = other.mem_attr;
        this->snp_attr = other.snp_attr;
        this->do_dwt = other.do_dwt;
        this->snoop_me = other.snoop_me;
        this->likely_shared = other.likely_shared;
        this->fwd_nid = other.fwd_nid;
        this->fwd_txnid = other.fwd_txnid;
        this->return_nid = other.return_nid;
        this->home_nid = other.home_nid;
        this->home_nid = other.home_nid;
        this->fwd_state = other.fwd_state;
        this->cbusy = other.cbusy;
        this->resp_err = other.resp_err;
        this->resp = other.resp;
        this->ret_to_src = other.ret_to_src;
        this->data_pull = other.data_pull;
        this->data_source = other.data_source;
        this->do_not_go_to_sd = other.do_not_go_to_sd;
        this->vmid_ext = other.vmid_ext;
        this->tag_op = other.tag_op;
        this->tag_group_id = other.tag_group_id;
        this->trace_tag = other.trace_tag;
        this->mpam = other.mpam;
        this->tu = other.tu;
        this->endian = other.endian;
        this->slc_rep_hint = other.slc_rep_hint;
        this->excl = other.excl;
        this->order = other.order;
        this->tag = other.tag;//may be err
        this->be = other.be;
        this->data_check = other.data_check;
        this->poison = other.poison;

    }
}}
