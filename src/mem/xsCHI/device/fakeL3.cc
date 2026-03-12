#include <cassert>
#include <cstdint>
#include <memory>
#include <string>

#include "fakeL3.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/CHIFakeL3.hh"
#include "mem/xsCHI/base/flit.hh"
#include "sim/clocked_object.hh"

namespace gem5
{
namespace xsCHI
{
FakeL3::FakeL3Stats::FakeL3Stats(FakeL3 *parent)
    : statistics::Group(parent, "protocol"),
      ADD_STAT(protocol_tx_by_opcode, statistics::units::Count::get(),
               "Protocol TX flits grouped by CHI opcode"),
      ADD_STAT(protocol_rx_by_opcode, statistics::units::Count::get(),
               "Protocol RX flits grouped by CHI opcode"),
      ADD_STAT(protocol_readshared_total, statistics::units::Count::get(),
               "Total ReadShared opcode observations"),
      ADD_STAT(protocol_writeevict_total, statistics::units::Count::get(),
               "Total WriteEvict opcode observations"),
      ADD_STAT(protocol_compack_total, statistics::units::Count::get(),
               "Total CompAck opcode observations"),
      ADD_STAT(protocol_snp_total, statistics::units::Count::get(),
               "Total snoop opcode observations")
{
    using namespace statistics;

    protocol_tx_by_opcode
        .init(FakeL3::NumChiOps)
        .flags(nozero);
    protocol_rx_by_opcode
        .init(FakeL3::NumChiOps)
        .flags(nozero);
    for (size_t i = 0; i < FakeL3::NumChiOps; ++i) {
        const auto op = static_cast<CHI_OP_TYPE>(i);
        const std::string label = CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op);
        protocol_tx_by_opcode.subname(i, label);
        protocol_rx_by_opcode.subname(i, label);
    }

    protocol_readshared_total.flags(nozero);
    protocol_writeevict_total.flags(nozero);
    protocol_compack_total.flags(nozero);
    protocol_snp_total.flags(nozero);
}

FakeL3::FakeL3(const Params &p)
    : ClockedObject(p), networkPort(p.networkPort), _NodeID(0), SAM(nullptr),
      TXN_Manager(1024), outstanding_requests(), stats(this)
{
    panic_if(networkPort == nullptr, "FakeL3 %s requires a valid networkPort",
             name());
    networkPort->setReceiveCallback(
        [this](FlitPtr &flit) { return this->handlePortReceive(flit); });
    networkPort->setOwner(this);
}

bool
FakeL3::handlePortReceive(FlitPtr &flit)
{
    panic_if(flit->getTgtId() != _NodeID,
             "FakeL3 %s received flit not targeting itself: tgt=%u self=%u "
             "op=%s",
             name(), flit->getTgtId(), _NodeID,
             CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()).c_str());
    recordProtocolRx(flit->getOpcode());

    switch (flit->get_Flit_Channel_Type()) {
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ: {
        switch (flit->getOpcode()) {
          case CHI_OP_TYPE::CHI_REQ_READUNIQUE:
          case CHI_OP_TYPE::CHI_REQ_READSHARED:
          case CHI_OP_TYPE::CHI_REQ_READCLEAN: {
            int txn_id = TXN_Manager.getID();
            if (txn_id < 0) {
                return false;
            }

            FlitPtr read = std::make_unique<Flit>(flit->getOpcode(),
                                                  flit->getAddr(),
                                                  flit->getSize());
            if (!read) {
                TXN_Manager.releaseID(txn_id);
                return false;
            }

            read->setOpcode(CHI_OP_TYPE::CHI_REQ_READNOSNP);
            read->setTgtId(SAM->getTargetID(flit->getAddr()));
            read->setSrcId(_NodeID);
            read->setTxnId(txn_id);
            read->setReturnNid(flit->getSrcId());
            read->setReturnTxnid(flit->getTxnId());

            ReqPtr req = std::make_shared<Request>(flit->getOpcode(),
                                                   flit->getAddr(),
                                                   flit->getSize());
            req->setSourceId(flit->getSrcId());
            req->setTargetId(read->getTgtId());
            req->setTransactionId(flit->getTxnId());

            const CHI_OP_TYPE txOp = read->getOpcode();
            if (networkPort->send(read)) {
                recordProtocolTx(txOp);
                saveOutstandingRequest(req, txn_id);
                return true;
            }

            TXN_Manager.releaseID(txn_id);
            return false;
          }
          case CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL:
          case CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL: {
            int txn_id = TXN_Manager.getID();
            if (txn_id < 0) {
                return false;
            }

            FlitPtr write = std::make_unique<Flit>(flit->getOpcode(),
                                                   flit->getAddr(),
                                                   flit->getSize());
            if (!write) {
                TXN_Manager.releaseID(txn_id);
                return false;
            }

            write->setOpcode(CHI_OP_TYPE::CHI_REQ_WRITENOSNPFULL);
            write->setTgtId(SAM->getTargetID(flit->getAddr()));
            write->setSrcId(_NodeID);
            write->setTxnId(txn_id);

            ReqPtr req = std::make_shared<Request>(flit->getOpcode(),
                                                   flit->getAddr(),
                                                   flit->getSize());
            req->setSourceId(flit->getSrcId());
            req->setTargetId(write->getTgtId());
            req->setTransactionId(flit->getTxnId());

            const CHI_OP_TYPE txOp = write->getOpcode();
            if (networkPort->send(write)) {
                recordProtocolTx(txOp);
                saveOutstandingRequest(req, txn_id);
                return true;
            }

            TXN_Manager.releaseID(txn_id);
            return false;
          }
          case CHI_OP_TYPE::CHI_REQ_EVICT: {
            FlitPtr rsp = std::make_unique<Flit>();
            rsp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMP);
            rsp->setSrcId(_NodeID);
            rsp->setTgtId(flit->getSrcId());
            rsp->setTxnId(flit->getTxnId());
            const CHI_OP_TYPE txOp = rsp->getOpcode();
            if (networkPort->send(rsp)) {
                recordProtocolTx(txOp);
                return true;
            }
            return false;
          }
          case CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE: {
            int txn_id = TXN_Manager.getID();
            if (txn_id < 0) {
                return false;
            }

            FlitPtr comp = std::make_unique<Flit>();
            if (!comp) {
                TXN_Manager.releaseID(txn_id);
                return false;
            }

            comp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMP);
            comp->setTgtId(flit->getSrcId());
            comp->setSrcId(_NodeID);
            comp->setTxnId(flit->getTxnId());
            comp->setDbid(txn_id);

            ReqPtr req = std::make_shared<Request>(flit->getOpcode(),
                                                   flit->getAddr(),
                                                   flit->getSize());
            req->setSourceId(flit->getSrcId());
            req->setTargetId(SAM->getTargetID(flit->getAddr()));
            req->setTransactionId(flit->getTxnId());

            const CHI_OP_TYPE txOp = comp->getOpcode();
            if (networkPort->send(comp)) {
                recordProtocolTx(txOp);
                saveOutstandingRequest(req, txn_id);
                return true;
            }

            TXN_Manager.releaseID(txn_id);
            return false;
          }
          default:
            assert(false && "Not supported yet");
            return false;
        }
      }
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP: {
        auto it = outstanding_requests.find(flit->getTxnId());
        assert(it != outstanding_requests.end());
        ReqPtr req = it->second;

        switch (flit->getOpcode()) {
          case CHI_OP_TYPE::CHI_RSP_COMPACK: {
            switch (req->getOpcode()) {
              case CHI_OP_TYPE::CHI_REQ_READUNIQUE:
              case CHI_OP_TYPE::CHI_REQ_READSHARED:
              case CHI_OP_TYPE::CHI_REQ_READCLEAN:
              case CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE:
                TXN_Manager.releaseID(flit->getTxnId());
                outstanding_requests.erase(it);
                DPRINTF(CHIFakeL3,
                        "Finish request by COMPACK: txn_id=%d, "
                        "outstanding_requests.size()=%d\n",
                        flit->getTxnId(), outstanding_requests.size());
                return true;
              default:
                assert(false);
                return false;
            }
          }
          case CHI_OP_TYPE::CHI_RSP_DBIDRESP: {
            switch (req->getOpcode()) {
              case CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL:
              case CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL: {
                FlitPtr rsp = std::make_unique<Flit>();
                rsp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMPDBIDRESP);
                rsp->setTgtId(req->getSourceId());
                rsp->setSrcId(_NodeID);
                rsp->setTxnId(req->getTransactionId());
                // RN 回程沿用 HN 内部事务号，后续 COPYBACK 用该值作为 txn。
                rsp->setDbid(flit->getTxnId());
                const CHI_OP_TYPE txOp = rsp->getOpcode();
                if (networkPort->send(rsp)) {
                    recordProtocolTx(txOp);
                    req->setDbid(flit->getDbid());
                    return true;
                }
                return false;
              }
              default:
                assert(false);
                return false;
            }
          }
          default:
            assert(false && "Unsupported RSP opcode");
            return false;
        }
      }
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA: {
        auto it = outstanding_requests.find(flit->getTxnId());
        assert(it != outstanding_requests.end());
        ReqPtr req = it->second;

        switch (flit->getOpcode()) {
          case CHI_OP_TYPE::CHI_DAT_COPYBACKWRDATA: {
            switch (req->getOpcode()) {
              case CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL:
              case CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL: {
                FlitPtr data = std::make_unique<Flit>();
                data->setOpcode(CHI_OP_TYPE::CHI_DAT_NCBWRDATACOMPACK);
                data->setTgtId(req->getTargetId());
                data->setSrcId(_NodeID);
                data->setTxnId(req->getDbid());
                data->setDbid(req->getDbid());
                data->setDataId(flit->getDataId());
                data->setCcid(0);
                data->setSize(flit->getSize());
                data->setAddr(req->getAddr());

                uint8_t *tmp = new uint8_t[flit->getSize()];
                flit->getData(tmp);
                data->setData(tmp);
                delete[] tmp;

                const CHI_OP_TYPE txOp = data->getOpcode();
                if (networkPort->send(data)) {
                    recordProtocolTx(txOp);
                    const uint32_t dataid = flit->getDataId();
                    req->finishTransferdata(dataid);
                    if (req->dataTransferFinished()) {
                        TXN_Manager.releaseID(flit->getTxnId());
                        outstanding_requests.erase(it);
                        DPRINTF(CHIFakeL3,
                                "Finish write request: txn_id=%d, "
                                "outstanding_requests.size()=%d\n",
                                flit->getTxnId(), outstanding_requests.size());
                    }
                    return true;
                }
                return false;
              }
              default:
                assert(false);
                return false;
            }
          }
          case CHI_OP_TYPE::CHI_DAT_COMPDATA: {
            // Compatibility path: if data is returned to HN, forward it to RN.
            switch (req->getOpcode()) {
              case CHI_OP_TYPE::CHI_REQ_READUNIQUE:
              case CHI_OP_TYPE::CHI_REQ_READSHARED:
              case CHI_OP_TYPE::CHI_REQ_READCLEAN: {
                FlitPtr fwd = std::make_unique<Flit>(*flit);
                fwd->setTgtId(req->getSourceId());
                fwd->setTxnId(req->getTransactionId());
                const CHI_OP_TYPE txOp = fwd->getOpcode();
                if (networkPort->send(fwd)) {
                    recordProtocolTx(txOp);
                    return true;
                }
                return false;
              }
              default:
                assert(false);
                return false;
            }
          }
          default:
            assert(false && "Unsupported DATA opcode");
            return false;
        }
      }
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
        assert(false && "Snoop handling not implemented yet");
        return false;
      default:
        assert(false && "Unknown Flit type");
        return false;
    }
}

void
FakeL3::saveOutstandingRequest(ReqPtr &req, uint32_t txn_id)
{
    assert(outstanding_requests.count(txn_id) == 0 &&
           "TxnID already used by another request");
    outstanding_requests[txn_id] = req;
    DPRINTF(CHIFakeL3,
            "Save outstanding request: txn_id=%d, outstanding_requests.size()=%d\n",
            txn_id, outstanding_requests.size());
}

CHIPort *
FakeL3::getNetworkPort()
{
    return networkPort;
}

void
FakeL3::init()
{
    return;
}

bool
FakeL3::isSnpOpcode(CHI_OP_TYPE op)
{
    return op > CHI_OP_TYPE::CHI_SNP_OP_START &&
           op < CHI_OP_TYPE::CHI_SNP_OP_END;
}

bool
FakeL3::isWriteEvictOpcode(CHI_OP_TYPE op)
{
    return op == CHI_OP_TYPE::CHI_REQ_WRITEEVICTFULL ||
           op == CHI_OP_TYPE::CHI_REQ_WRITEEVICTOREVICT;
}

size_t
FakeL3::opcodeToIndex(CHI_OP_TYPE op)
{
    return static_cast<size_t>(op);
}

void
FakeL3::updateProtocolAliases(CHI_OP_TYPE op)
{
    if (op == CHI_OP_TYPE::CHI_REQ_READSHARED) {
        stats.protocol_readshared_total++;
    }
    if (isWriteEvictOpcode(op)) {
        stats.protocol_writeevict_total++;
    }
    if (op == CHI_OP_TYPE::CHI_RSP_COMPACK) {
        stats.protocol_compack_total++;
    }
    if (isSnpOpcode(op)) {
        stats.protocol_snp_total++;
    }
}

void
FakeL3::recordProtocolTx(CHI_OP_TYPE op)
{
    const size_t idx = opcodeToIndex(op);
    if (idx < NumChiOps) {
        stats.protocol_tx_by_opcode[idx]++;
    }
    updateProtocolAliases(op);
}

void
FakeL3::recordProtocolRx(CHI_OP_TYPE op)
{
    const size_t idx = opcodeToIndex(op);
    if (idx < NumChiOps) {
        stats.protocol_rx_by_opcode[idx]++;
    }
    updateProtocolAliases(op);
}

} // namespace xsCHI
} // namespace gem5
