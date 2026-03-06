#include <cassert>
#include <cstdint>
#include <memory>

#include "HNF.hh"
#include "base/trace.hh"
#include "mem/xsCHI/base/flit.hh"
#include "sim/clocked_object.hh"
#include "debug/CHIFakeL3.hh"

namespace gem5
{
namespace xsCHI
{
    FakeL3::FakeL3(const Params &p):
    ClockedObject(p),
    L2side(p.L2side),
    Dramside(p.Dramside),_NodeID(0),SAM(nullptr),TXN_Manager(1024){
        L2side->setReceiveCallback([this](FlitPtr &flit) { return this->handleL2sideRecv(flit); });
        L2side->setOwner(this);
        Dramside->setReceiveCallback([this](FlitPtr &flit) { return this->handleDramsideRecv(flit);});
        Dramside->setOwner(this);
    }

    // FakeL3::FakeL3(const Params &p,NodeID id,SystemAddressMap *sam):
    // ClockedObject(p),
    // L2side(p,this,this->name()+"_l2side",4),
    // Dramside(p,this,this->name()+"_Dramside",4),_NodeID(id),SAM(sam),TXN_Manager(1024){
    //     L2side->setReceiveCallback([this](FlitPtr &flit) { return this->handleL2sideRecv(flit); });
    //     Dramside->setReceiveCallback([this](FlitPtr &flit) { return this->handleDramsideRecv(flit);});
    // }

    bool
    FakeL3::handleL2sideRecv(FlitPtr &flit){
        switch (flit->get_Flit_Channel_Type()) {
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ: {
                switch (flit->getOpcode()) {
                    case CHI_OP_TYPE::CHI_REQ_READUNIQUE:
                    case CHI_OP_TYPE::CHI_REQ_READSHARED:
                    case CHI_OP_TYPE::CHI_REQ_READCLEAN:{
                        // 处理读取请求的响应
                        int txn_id = TXN_Manager.getID();
                        if (txn_id < 0) {
                            return false;
                        }else{
                            FlitPtr read = std::make_unique<Flit>(flit->getOpcode(),flit->getAddr(),flit->getSize());
                            if (!read) {
                                return false; // 创建失败
                            }
                            // 设置Flit的相关字段
                            read->setOpcode(CHI_OP_TYPE::CHI_REQ_READNOSNP);
                            read->setTgtId(SAM->getTargetID(flit->getAddr()));
                            read->setSrcId(_NodeID);
                            read->setTxnId(txn_id);
                            read->setReturnNid(flit->getSrcId());
                            read->setReturnTxnid(flit->getTxnId());
                            ReqPtr req = std::make_shared<Request>(
                                flit->getOpcode(),flit->getAddr(),flit->getSize());
                            // 尝试发送到网络端口
                            if (Dramside->send(read)){
                                //send success, we need to save the request and txn_id
                                saveOutstandingRequest(req, txn_id);
                                return true;
                            }else{
                                //send failed, we need to save the request and retry later
                                if (read!=nullptr) {
                                    read.reset();
                                }
                                TXN_Manager.releaseID(txn_id);
                                return false;
                            }
                        }
                    }
                    case CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL:
                    case CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL:{

                        int txn_id = TXN_Manager.getID();
                        if (txn_id < 0) {
                            return false;
                        }else{
                            FlitPtr write = std::make_unique<Flit>(flit->getOpcode(),flit->getAddr(),flit->getSize());// just for addr and size
                            if (!write) {
                                return false; // 创建失败
                            }
                            // 设置Flit的相关字段
                            write->setOpcode(CHI_OP_TYPE::CHI_REQ_WRITENOSNPFULL);
                            write->setTgtId(SAM->getTargetID(flit->getAddr()));
                            write->setSrcId(_NodeID);
                            write->setTxnId(txn_id);

                            ReqPtr req = std::make_shared<Request>(
                                flit->getOpcode(),flit->getAddr(),flit->getSize());
                            req->setSourceId(flit->getSrcId());
                            req->setTargetId(write->getTgtId());
                            req->setTransactionId(flit->getTxnId());
                            // 尝试发送到网络端口
                            if (Dramside->send(write)){
                                //send success, we need to save the request and txn_id
                                saveOutstandingRequest(req, txn_id);
                                return true;
                            }else{
                                //send failed, we need to save the request and retry later
                                if (write!=nullptr) {
                                    write.reset();
                                }
                                TXN_Manager.releaseID(txn_id);
                                return false;
                            }
                        }
                    }
                    case CHI_OP_TYPE::CHI_REQ_EVICT:{
                        // 处理驱逐请求的响应
                        //send ack back
                        FlitPtr rsp = std::make_unique<Flit>();
                        rsp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMP);
                        rsp->setSrcId(_NodeID);
                        rsp->setTgtId(flit->getSrcId());
                        rsp->setTxnId(flit->getTxnId());
                        if (L2side->send(rsp)){
                            return true;
                        }else{
                            if (rsp!=nullptr) {
                                rsp.reset();
                            }
                            return false;
                        }
                    }
                    case CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE:{
                        // 处理清理唯一性请求的响应
                        int txn_id = TXN_Manager.getID();
                        if (txn_id < 0) {
                            return false;
                        }else{
                            FlitPtr comp = std::make_unique<Flit>();
                            if (!comp) {
                                return false; // 创建失败
                            }
                            // 设置Flit的相关字段
                            comp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMP);
                            comp->setTgtId(flit->getSrcId());
                            comp->setSrcId(_NodeID);
                            comp->setTxnId(flit->getTxnId());
                            comp->setDbid(txn_id);
                            ReqPtr req = std::make_shared<Request>(
                                flit->getOpcode(),flit->getAddr(),flit->getSize());
                            // 尝试发送到网络端口
                            if (Dramside->send(comp)){
                                //send success, we need to save the request and txn_id
                                saveOutstandingRequest(req, txn_id);
                                return true;
                            }else{
                                //send failed, we need to save the request and retry later
                                if (comp!=nullptr) {
                                    comp.reset();
                                }
                                TXN_Manager.releaseID(txn_id);
                                return false;
                            }
                        }
                    }
                    default: {
                        assert(false && "Not supported yet");
                        return false; // 处理失败
                    }
                }
            }
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP: {
                // 处理响应Flit
                // this response Flit must be a response to a request we sent out.
                assert(TXN_Manager.isUsed(flit->getTxnId()));
                ReqPtr req = outstanding_requests[flit->getTxnId()];
                switch (req->getOpcode()) {
                    case CHI_OP_TYPE::CHI_REQ_READUNIQUE:
                    case CHI_OP_TYPE::CHI_REQ_READSHARED:
                    case CHI_OP_TYPE::CHI_REQ_READCLEAN:{
                        switch (flit->getOpcode()) {
                            case CHI_OP_TYPE::CHI_RSP_COMPACK:{
                                // assert(req->dataTransferFinished());
                                TXN_Manager.releaseID(flit->getTxnId());
                                outstanding_requests.erase(flit->getTxnId());
                                DPRINTF(CHIFakeL3, "Finish read request: txn_id=%d, outstanding_requests.size()=%d\n", flit->getTxnId(), outstanding_requests.size());
                                return true;
                            }
                            default:{
                                assert(false);
                            }
                        }
                    case CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE:{
                        switch (flit->getOpcode()) {
                            case CHI_OP_TYPE::CHI_RSP_COMPACK:{
                                TXN_Manager.releaseID(flit->getTxnId());
                                outstanding_requests.erase(flit->getTxnId());
                                DPRINTF(CHIFakeL3, "Finish clean unique request: txn_id=%d, outstanding_requests.size()=%d\n", flit->getTxnId(), outstanding_requests.size());
                                return true;
                            }
                            default:{
                                assert(false);
                            }
                        }
                    }

                    }
                    default:{
                        assert(false);
                    }

                }


            }
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP: {
                // HN不应该收到 Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP 类型的Flit
                assert(false && "Received a Snoop Flit on network port, which should not happen in HN");
                return false;
            }
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA: {
                // 处理数据Flit
                // this response Flit must be a response to a request we sent out.
                assert(TXN_Manager.isUsed(flit->getTxnId()));
                switch (outstanding_requests[flit->getTxnId()]->getOpcode()) {

                    case CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL:
                    case CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL:{
                        switch (flit->getOpcode()) {

                            case CHI_OP_TYPE::CHI_DAT_COPYBACKWRDATA:{
                                // 处理读取请求的响应
                                //recv and send to dram!
                                ReqPtr req = outstanding_requests[flit->getTxnId()];

                                FlitPtr data = std::make_unique<Flit>();
                                if (!data) {
                                    return false; // 创建失败
                                }
                                // 设置Flit的相关字段
                                data->setOpcode(CHI_OP_TYPE::CHI_DAT_NCBWRDATACOMPACK);
                                data->setTgtId(req->getTargetId());
                                data->setSrcId(_NodeID);
                                data->setTxnId(req->getDbid());
                                data->setDataId(flit->getDataId());
                                data->setCcid(0); // assuming CCID is always 0
                                data->setSize(flit->getSize());
                                uint8_t *tmp = new uint8_t[flit->getSize()];
                                flit->getData(tmp);
                                data->setData(tmp);
                                delete[] tmp; // 释放临时内存
                                
                                // 尝试发送到网络端口
                                if (Dramside->send(data)){
                                    //send success, we need to save the request and txn_id
                                    uint32_t dataid = flit->getDataId();
                                    req->finishTransferdata(dataid);
                                    if (req->dataTransferFinished()){
                                        TXN_Manager.releaseID(flit->getTxnId());
                                        outstanding_requests.erase(flit->getTxnId());
                                        DPRINTF(CHIFakeL3, "Finish write request: txn_id=%d, outstanding_requests.size()=%d\n", flit->getTxnId(), outstanding_requests.size());
                                    }
                                    return true;
                                }else{
                                    //send failed, we need to save the request and retry later
                                    if (data!=nullptr) {
                                        data.reset();
                                    }
                                    return false;
                                }




                            }
                            default: {
                                assert(false && "Not supported yet");
                                return false; // 处理失败
                            }
                        }
                    default:
                        assert(false);
                    }
                }
            }
            default:
                assert(false && "Unknown Flit type");
                return false; // 处理失败
        }
        return true;
    }

    bool
    FakeL3::handleDramsideRecv(FlitPtr &flit){
        if(flit->getTgtId()!= _NodeID){
            // 不是发给本节点的Flit,要转发给RN
            FlitPtr copy = std::make_unique<Flit>(*flit);
            if (L2side->send(copy)){
                //send success
                return true;
            }else{
                //send failed, 
                if (copy!=nullptr) {
                    copy.reset();
                }
                return false;
            }
            return false;
        }
        switch (flit->get_Flit_Channel_Type()) {
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA: {
                ReqPtr req = outstanding_requests[flit->getTxnId()];
                switch (req->getOpcode()) {
                    case CHI_OP_TYPE::CHI_REQ_READUNIQUE:
                    case CHI_OP_TYPE::CHI_REQ_READSHARED:
                    case CHI_OP_TYPE::CHI_REQ_READCLEAN:{
                        switch (flit->getOpcode()) {
                            case CHI_OP_TYPE::CHI_DAT_COMPDATA:{

                                FlitPtr rsp = std::make_unique<Flit>(*flit);//here need to call copy constructor!!
                                if (!rsp) {
                                    return false; // 创建失败
                                }
                                // 尝试发送到网络端口
                                if (L2side->send(rsp)){
                                    //send success, we need to save the request and txn_id
                                    req->finishTransferdata(flit->getDataId());
                                    return true;
                                }else{
                                    //send failed, we need to save the request and retry later
                                    if (rsp!=nullptr) {
                                        rsp.reset();
                                    }
                                    return false;
                                }

                            }
                            default:
                                assert(false && "Unknown Flit type");
                        }
                    }
                    default:{
                        assert(false);
                    }
                }


            }
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP: {
                ReqPtr req = outstanding_requests[flit->getTxnId()];
                switch (req->getOpcode()) {
                    case CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL:
                    case CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL:{
                        switch (flit->getOpcode()) {
                            case CHI_OP_TYPE::CHI_RSP_DBIDRESP:{

                                FlitPtr rsp = std::make_unique<Flit>();
                                if (!rsp) {
                                    return false; // 创建失败
                                }
                                // 设置Flit的相关字段
                                rsp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMPDBIDRESP);
                                rsp->setTgtId(req->getSourceId());
                                rsp->setSrcId(_NodeID);
                                rsp->setTxnId(req->getTransactionId());
                                rsp->setDbid(flit->getTxnId());//here we make send and recv use the same transaction!
                                // 尝试发送到网络端口
                                if (L2side->send(rsp)){
                                    //send success, we need to save the request and txn_id
                                    req->setDbid(flit->getDbid());
                                    return true;
                                }else{
                                    //send failed, we need to save the request and retry later
                                    if (rsp!=nullptr) {
                                        rsp.reset();
                                    }
                                    return false;
                                }

                            }
                            default:
                                assert(false && "Unknown Flit type");
                        }
                    }
                    default:{
                        assert(false);
                    }
                }

            }
            default:{
                assert(false && "Unknown Flit type");
            }
        }
        return true;
    }

    void FakeL3::saveOutstandingRequest(ReqPtr &req, uint32_t txn_id)
    {
        // 保存未完成的请求
        //make sure the txn_id is not used by previous request
         assert(outstanding_requests.count(txn_id) == 0 &&
                "TxnID already used by another request");
        outstanding_requests[txn_id] = req;
        DPRINTF(CHIFakeL3, "Save outstanding request: txn_id=%d, outstanding_requests.size()=%d\n", txn_id, outstanding_requests.size());
    }
    CHIPort*
    FakeL3::getCHIPort_CPUSIDE(){
        return L2side;
    }
    CHIPort*
    FakeL3::getCHIPort_MEMSIDE(){
        return Dramside;
    }
    void FakeL3::init(){
        return;
    }
}}
