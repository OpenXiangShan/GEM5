#include "mem/xsCHI/base/flit.hh"

#include <cstdint>

#include "mem/xsCHI/base/request.hh"

namespace gem5 {
namespace xsCHI {
    void
    Flit::init()
    {
        tgt_id = 0;
        src_id = 0;
        home_nid = 0;
        return_nid = 0;
        fwd_nid = 0;
        lpid = 0;
        pgroup_id = 0;
        stash_nid = 0;
        stash_nid_valid = false;
        stash_lpid = 0;
        stash_lpid_valid = false;
        stash_group_id = 0;
        txn_id = 0;
        return_txnid = 0;
        fwd_txnid = 0;
        dbid = 0;
        opcode = CHI_OP_TYPE::CHI_REQ_OP_START;
        addr = 0;
        size = 0;
        data = nullptr;
        ccid = 0;
        data_id = 0;
    }

    Flit::Flit(FlitPtr other){
        init();
        opcode = other->opcode;
        size = other->getSize();
        addr = other->getAddr();
        data = nullptr;
        if (other->DataValid()){
            data = new uint8_t[other->getSize()];
            other->getData(data);
        }
        tgt_id = other->tgt_id;
        src_id = other->src_id;
        home_nid = other->home_nid;
        return_nid = other->return_nid;
        fwd_nid = other->fwd_nid;
        lpid = other->lpid;
        pgroup_id = other->pgroup_id;
        stash_nid = other->stash_nid;
        stash_nid_valid = other->stash_nid_valid;
        stash_lpid = other->stash_lpid;
        stash_lpid_valid = other->stash_lpid_valid;
        stash_group_id = other->stash_group_id;
        txn_id = other->txn_id;
        return_txnid = other->return_txnid;
        fwd_txnid = other->fwd_txnid;
        dbid = other->dbid;
        ccid = other->ccid;
        data_id = other->data_id;

    }
    Flit::Flit(CHI_OP_TYPE op,uint64_t addr,uint32_t size){
        init();
        opcode = op;
        this->size = size;
        this->addr = addr;
        data = nullptr;
    }
    void
    Flit::setData(ReqPtr req)
    {
        assert(opcode >= CHI_OP_TYPE::CHI_DAT_OP_START &&
                opcode <= CHI_OP_TYPE::CHI_DAT_OP_END &&
                "Flit must be a data flit to set data");
        assert(getSize() > 0 && "Flit data size must be greater than 0");
        assert(req->getSize() == getSize());
        assert(req->DataValid() && "Destination pointer for data must not be null");
        if (!DataValid()){
            // allocate memory for data
            data = new uint8_t[getSize()];
        }
        req->getData(data);
    }
}
}
