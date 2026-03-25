#include "mem/xsCHI/device/CHI_L3.hh"

#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <csignal>
#include <deque>

#include "base/addr_range.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/CHIL3.hh"
#include "debug/CHIL3Txn.hh"
#include "mem/packet.hh"
#include "mem/xsCHI/base/request.hh"
#include "sim/system.hh"

namespace gem5
{
namespace xsCHI
{

CHI_L3::CHI_L3(const Params &p)
        : ClockedObject(p),
            dataSendEvent([this] { this->drainDataQueue(); },
                          csprintf("%s.data_send", name())),
            compRspSendEvent([this] { this->drainCompRspQueue(); },
                             csprintf("%s.comp_rsp_send", name())),
            writeDataSendEvent([this] { this->drainWriteDataQueue(); },
                               csprintf("%s.write_data_send", name())),
            pendingXbarSendEvent([this] { this->drainPendingXbarQueue(); },
                                 csprintf("%s.pending_xbar_send", name())),
            pendingDdrSendEvent([this] { this->drainPendingDdrQueue(); },
                                csprintf("%s.pending_ddr_send", name())),
            pendingCacheMemReqSendEvent(
                [this] { this->drainPendingCacheMemReqQueue(); },
                csprintf("%s.pending_cache_mem_req_send", name())),
            networkPort(p.networkPort),
            cacheWrapper(p.cache_wrapper),
            coherentXBar(p.coherent_xbar),
            innerCacheReqPort(csprintf("%s.inner_cache_req", name()), this),
            innerCacheRespPort(csprintf("%s.inner_cache_resp", name()), this)
{
    fatal_if(!networkPort,
             "CHI_L3 requires networkPort CHIPort");
    networkPort->setReceiveCallback(
        [this](FlitPtr &flit) { return this->handleNetworkFlit(flit); });
    networkPort->setCreditUnblockCallback(
        [this](Flit::CHI_CHN_TYPE channel) { handleCreditUnblock(channel); });
    networkPort->setOwner(this);
}

void
CHI_L3::init()
{
    ClockedObject::init();

    fatal_if(!cacheWrapper, "CHI_L3 needs CacheWrapper instance");
    fatal_if(!coherentXBar, "CHI_L3 needs coherent_xbar instance");

    // Ensure xbar learns address coverage from inner response side.
    innerCacheRespPort.sendRangeChange();
}

Port &
CHI_L3::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "inner_req_port") {
        return innerCacheReqPort;
    }
    if (if_name == "inner_resp_port") {
        return innerCacheRespPort;
    }
    return ClockedObject::getPort(if_name, idx);
}

Addr
CHI_L3::blockAddr(Addr addr) const
{
    return addr & ~static_cast<Addr>(0x3f);
}

void
CHI_L3::trackPendingRead(Addr addr)
{
    const Addr blk = blockAddr(addr);
    pendingReadCount[blk]++;
    DPRINTF(CHIL3,
            "track pending read blk=%#lx count=%u\n",
            blk, pendingReadCount[blk]);
}

void
CHI_L3::completePendingRead(Addr addr)
{
    const Addr blk = blockAddr(addr);
    auto it = pendingReadCount.find(blk);
    if (it == pendingReadCount.end()) {
        return;
    }
    if (it->second > 0) {
        it->second--;
    }
    DPRINTF(CHIL3,
            "complete pending read blk=%#lx count=%u\n",
            blk, it->second);
    if (it->second == 0) {
        pendingReadCount.erase(it);
        wakeBlockedWrites(blk);
    }
}

bool
CHI_L3::hasPendingRead(Addr addr) const
{
    const Addr blk = blockAddr(addr);
    auto it = pendingReadCount.find(blk);
    return it != pendingReadCount.end() && it->second > 0;
}

void
CHI_L3::trackPendingWrite(Addr addr)
{
    const Addr blk = blockAddr(addr);
    pendingWriteCount[blk]++;
    DPRINTF(CHIL3,
            "track pending write blk=%#lx count=%u\n",
            blk, pendingWriteCount[blk]);
}

void
CHI_L3::completePendingWrite(Addr addr)
{
    const Addr blk = blockAddr(addr);
    auto it = pendingWriteCount.find(blk);
    if (it == pendingWriteCount.end()) {
        return;
    }
    if (it->second > 0) {
        it->second--;
    }
    DPRINTF(CHIL3,
            "complete pending write blk=%#lx count=%u\n",
            blk, it->second);
    if (it->second == 0) {
        pendingWriteCount.erase(it);
        wakeBlockedReads(blk);
    }
}

bool
CHI_L3::hasPendingWrite(Addr addr) const
{
    const Addr blk = blockAddr(addr);
    auto it = pendingWriteCount.find(blk);
    return it != pendingWriteCount.end() && it->second > 0;
}

void
CHI_L3::enqueueBlockedRead(PacketPtr pkt, uint32_t txnId)
{
    const Addr blk = blockAddr(pkt->getAddr());
    blockedReadByAddr[blk].push_back({pkt, txnId});
    DPRINTF(CHIL3,
            "block read txn=%u addr=%#lx blk=%#lx pending_write=%u blocked_reads=%u\n",
            txnId, pkt->getAddr(), blk,
            pendingWriteCount[blk],
            static_cast<unsigned>(blockedReadByAddr[blk].size()));
}

bool
CHI_L3::dispatchReadToXbar(PacketPtr pkt, uint32_t txnId)
{
    bool cleanupTxn = false;
    auto metaIt = txnTable.find(txnId);
    if (metaIt != txnTable.end()) {
        cleanupTxn = metaIt->second.retireAfterXbarSend;
    }

    if (xbarRetryPending || !pendingXbarQ.empty()) {
        DPRINTF(CHIL3,
                "xbar unavailable (retryPending=%d queue=%u), enqueue addr=%#lx\n",
                xbarRetryPending,
                static_cast<unsigned>(pendingXbarQ.size()),
                pkt->getAddr());
        enqueuePendingXbar(pkt, cleanupTxn, txnId);
        return true;
    }

    if (!sendPktToXbar(pkt)) {
        xbarRetryPending = true;
        DPRINTF(CHIL3,
                "send to xbar blocked addr=%#lx, queue retry\n",
                pkt->getAddr());
        enqueuePendingXbar(pkt, cleanupTxn, txnId);
    } else {
        // completePendingRead(pkt->getAddr());
        if (cleanupTxn) {
            // Handle CLEANUNIQUE completion: send CHI_RSP_COMP and wait COMPACK
            assert(metaIt->second.req->getOpcode() == CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE ||
                   metaIt->second.req->getOpcode() == CHI_OP_TYPE::CHI_REQ_EVICT );
            if (metaIt->second.req->getOpcode() == CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE){
                assert(pkt->cacheResponding());
            }
            DPRINTF(CHIL3,
                    "Due to UpgradeReq is set iscacheResponding send CompREP after \
                    dispatched CLEANUNIQUE to xbar, enqueue comp rsp txn=%u addr=%#lx\n",
                    txnId, pkt->getAddr());
            pendingCompRspQ.push_back(txnId);
            scheduleNetworkRetry(
                compRspSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
            return true;
        }
    }
    return true;
}

bool
CHI_L3::dispatchWriteToXbar(PacketPtr pkt, uint32_t txnId)
{
    //check if pendingXbarQ has requests to same block addr,
    // if yes, we need to enqueue this write req to pendingXbarQ to ensure ordering,
    // otherwise we can directly send to xbar
    bool hasSameBlockPendingXbarReq = false;
    const Addr blk = blockAddr(pkt->getAddr());
    for (const auto &pendingReq : pendingXbarQ) {
        if (blockAddr(pendingReq.pkt->getAddr()) == blk) {
            hasSameBlockPendingXbarReq = true;
            break;
        }
    }
    if (hasSameBlockPendingXbarReq) {
        DPRINTF(CHIL3,
                "has same block pending xbar req, enqueue write txn=%u addr=%#lx\n",
                txnId, pkt->getAddr());
        enqueuePendingXbar(pkt, /*cleanupTxn*/ true, txnId);
        return true;
    }
    enqueuePendingXbar(pkt, /*cleanupTxn*/ true, txnId);
    return true;
}

void
CHI_L3::wakeBlockedReads(Addr addr)
{
    const Addr blk = blockAddr(addr);
    auto it = blockedReadByAddr.find(blk);
    if (it == blockedReadByAddr.end()) {
        return;
    }

    auto &queue = it->second;
    DPRINTF(CHIL3,
            "wake blocked reads blk=%#lx count=%u\n",
            blk, static_cast<unsigned>(queue.size()));

    while (!queue.empty()) {
        PendingReadReq pending = queue.front();
        queue.pop_front();
        DPRINTF(CHIL3,
                "wake read txn=%u addr=%#lx\n",
                pending.txnId, pending.pkt->getAddr());
        dispatchReadToXbar(pending.pkt, pending.txnId);
    }

    blockedReadByAddr.erase(it);
}

    void
    CHI_L3::enqueueBlockedWrite(PacketPtr pkt, uint32_t txnId)
    {
        const Addr blk = blockAddr(pkt->getAddr());
        blockedWriteByAddr[blk].push_back({pkt, txnId});
        DPRINTF(CHIL3,
            "block write txn=%u addr=%#lx blk=%#lx pending_read=%u blocked_writes=%u\n",
            txnId,
            pkt->getAddr(),
            blk,
            pendingReadCount[blk],
            static_cast<unsigned>(blockedWriteByAddr[blk].size()));
    }

    void
    CHI_L3::wakeBlockedWrites(Addr addr)
    {
        const Addr blk = blockAddr(addr);
        auto it = blockedWriteByAddr.find(blk);
        if (it == blockedWriteByAddr.end()) {
        return;
        }

        auto &queue = it->second;
        DPRINTF(CHIL3,
            "wake blocked writes blk=%#lx count=%u\n",
            blk, static_cast<unsigned>(queue.size()));

        while (!queue.empty()) {
        PendingWriteReq pending = queue.front();
        queue.pop_front();
        DPRINTF(CHIL3,
            "wake write txn=%u addr=%#lx\n",
            pending.txnId, pending.pkt->getAddr());
        enqueuePendingXbar(pending.pkt, /*cleanupTxn*/ true, pending.txnId);
        }

        blockedWriteByAddr.erase(it);
    }

void
CHI_L3::trackCacheReqTxn(PacketPtr pkt, Addr addr, uint32_t txnId)
{
    CacheReqKey key{pkt, addr};
    cacheReqMap[key] = txnId;
    DPRINTF(CHIL3Txn,
            "cacheReqMap track pkt=%p addr=%#lx txn=%u\n",
            pkt, addr, txnId);
}

uint32_t
CHI_L3::peekCacheReqTxn(PacketPtr pkt, Addr addr) const
{
    CacheReqKey key{pkt, addr};
    auto it = cacheReqMap.find(key);
    if (it == cacheReqMap.end()) {
        return TxnIDManager::InvalidTxnId;
    }
    return it->second;
}

bool
CHI_L3::popCacheReqTxn(PacketPtr pkt, Addr addr, uint32_t &txnId)
{
    CacheReqKey key{pkt, addr};
    auto it = cacheReqMap.find(key);
    if (it == cacheReqMap.end()) {
        return false;
    }
    txnId = it->second;
    cacheReqMap.erase(it);
    return true;
}

bool
CHI_L3::eraseCacheReqTxn(PacketPtr pkt, Addr addr, uint32_t txnId)
{
    CacheReqKey key{pkt, addr};
    auto it = cacheReqMap.find(key);
    if (it == cacheReqMap.end()) {
        return false;
    }
    if (it->second != txnId) {
        return false;
    }
    cacheReqMap.erase(it);
    DPRINTF(CHIL3Txn,
            "cacheReqMap erase pkt=%p addr=%#lx txn=%u\n",
            pkt, addr, txnId);
    return true;
}
bool
CHI_L3::handleNetworkFlit(FlitPtr &flit)
{
    const CHI_OP_TYPE op = flit->getOpcode();
    if (op == CHI_OP_TYPE::CHI_RSP_DBIDRESP ||
        op == CHI_OP_TYPE::CHI_DAT_COMPDATA) {
        return handleMemSideFlit(flit);
    }
    return handleCpuSideFlit(flit);
}

bool
CHI_L3::handleCpuSideFlit(FlitPtr &flit)
{
    DPRINTF(CHIL3, "cpuSide recv flit opcode=%s addr=%#lx txn=%u\n",
            CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()),
            flit->getAddr(), flit->getTxnId());

    switch (flit->get_Flit_Channel_Type()) {
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ: {
          CHI_OP_TYPE op = flit->getOpcode();
          switch (op) {
            case CHI_OP_TYPE::CHI_REQ_READUNIQUE:
            case CHI_OP_TYPE::CHI_REQ_READSHARED:
            case CHI_OP_TYPE::CHI_REQ_READCLEAN:{
                // Allocate txn tracking for requests that expect responses.
                uint32_t txnId = allocateTxnId();
                if (txnId == TxnIDManager::InvalidTxnId) {
                    DPRINTF(CHIL3, "No free TxnID available for new read request, opcode=%s addr=%#lx\n",
                            CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op), flit->getAddr());
                    return false;
                }
                PacketPtr pkt = nullptr;
                MemCmd cmd = mapChiReqToMemCmd(op);
                auto req = std::make_shared<gem5::Request>(
                    flit->getAddr(), flit->getSize(), Flags<uint64_t>(0),
                    0);
                req->setPaddr(flit->getAddr());
                pkt = new Packet(req, cmd, flit->getSize());
                // Data will be provided later via COPYBACKWRDATA flits.
                pkt->allocate();
                if (flit->getCacheResponding()) {
                    pkt->setCacheResponding();
                    pkt->setExpressSnoop();
                }
                if (flit->getResponderHadWritable()) {
                    pkt->setResponderHadWritable();
                }
                TxnMeta meta;
                meta.opcode = op;
                meta.addr = flit->getAddr();
                meta.size = flit->getSize();
                meta.srcId = flit->getSrcId();
                // meta.returnNid = flit->getReturnNid();
                // meta.returnTxnId = flit->getReturnTxnid();
                meta.txnId = flit->getTxnId();
                meta.dbid = txnId;
                meta.pkt = pkt;
                meta.dataBits.assign((flit->getSize() + 31) / 32, false);
                meta.req = std::make_shared<Request>(op, flit->getAddr(), flit->getSize());
                meta.cacheResponding = flit->getCacheResponding();
                meta.responderHadWritable = flit->getResponderHadWritable();
                txnTable[txnId] = meta;
                DPRINTF(CHIL3Txn,
                    "txnTable insert key=%u reason=cpu_req_track opcode=%s addr=%#lx size=%u src=%u retTxn=%u size_now=%u\n",
                    txnId, CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op),
                    flit->getAddr(), flit->getSize(), flit->getSrcId(),
                    flit->getReturnTxnid(), static_cast<unsigned>(txnTable.size()));
                trackCacheReqTxn(pkt, flit->getAddr(), txnId);
                trackPendingRead(pkt->getAddr());
                if (hasPendingWrite(pkt->getAddr())) {
                    enqueueBlockedRead(pkt, txnId);
                } else {
                    dispatchReadToXbar(pkt, txnId);
                }
                return true;
                break;
            }
            case CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL:
            case CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL:{
                // Writeback path: first return DBIDRESP, then wait COPYBACKWRDATA.
                const uint32_t dbid = allocateTxnId();
                if (dbid == TxnIDManager::InvalidTxnId) {
                    DPRINTF(CHIL3, "No free TxnID available for new writeback request, opcode=%s addr=%#lx\n",
                            CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op), flit->getAddr());
                    return false;
                }
                auto req = std::make_shared<gem5::Request>(
                    flit->getAddr(), flit->getSize(), Flags<uint64_t>(0),
                    0);
                req->setPaddr(flit->getAddr());
                PacketPtr pkt = new Packet(req, mapChiReqToMemCmd(op),
                                            flit->getSize());
                pkt->allocate();

                TxnMeta meta;
                meta.opcode = op;
                meta.addr = flit->getAddr();
                meta.size = flit->getSize();
                meta.srcId = flit->getSrcId();
                meta.returnTxnId = flit->getTxnId(); // original cpu txn
                meta.dbid = dbid;
                meta.pkt = pkt;
                meta.dataBits.assign((flit->getSize() + 31) / 32, false);
                meta.req = std::make_shared<Request>(op, flit->getAddr(),
                                                    flit->getSize());
                meta.req->setTransactionId(dbid);
                txnTable[dbid] = meta;
                trackPendingWrite(flit->getAddr());
                DPRINTF(CHIL3Txn,
                    "txnTable insert key=%u reason=cpu_writeback_req opcode=%s addr=%#lx size=%u src=%u retTxn=%u size_now=%u\n",
                    dbid, CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op), flit->getAddr(),
                    flit->getSize(), flit->getSrcId(), flit->getTxnId(),
                    static_cast<unsigned>(txnTable.size()));

                FlitPtr resp = std::make_unique<Flit>();
                resp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMPDBIDRESP);
                resp->setDbid(dbid);
                resp->setSrcId(_NodeID);
                resp->setTgtId(flit->getSrcId());
                resp->setTxnId(flit->getTxnId());

                DPRINTF(CHIL3,
                        "cpuSide send DBIDRESP src=%u tgt=%u txn=%u dbid=%u addr=%#lx size=%u\n",
                        _NodeID, flit->getSrcId(), flit->getTxnId(), dbid,
                        flit->getAddr(), flit->getSize());

                if (!networkPort->send(resp)) {
                    DPRINTF(CHIL3,
                            "cpuSide send DBIDRESP blocked txn=%u dbid=%u\n",
                            flit->getTxnId(), dbid);
                    txnTable.erase(dbid);
                    DPRINTF(CHIL3Txn,
                        "txnTable erase key=%u reason=dbidresp_send_blocked size_before=%u\n",
                        dbid, static_cast<unsigned>(txnTable.size()));
                    DPRINTF(CHIL3Txn,
                        "txnTable size_after=%u\n",
                        static_cast<unsigned>(txnTable.size()));
                    releaseTxn(dbid);
                    completePendingWrite(flit->getAddr());
                    return false;
                }
                return true;
                break;
            }
            case CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE:{
                // Allocate txn tracking for requests that expect responses.
                uint32_t txnId = allocateTxnId();
                if (txnId == TxnIDManager::InvalidTxnId) {
                    DPRINTF(CHIL3, "No free TxnID available for new clean unique request, opcode=%s addr=%#lx\n",
                            CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op), flit->getAddr());
                    return false;
                }
                PacketPtr pkt = nullptr;
                MemCmd cmd = mapChiReqToMemCmd(op);
                auto req = std::make_shared<gem5::Request>(
                    flit->getAddr(), flit->getSize(), Flags<uint64_t>(0),
                    0);
                req->setPaddr(flit->getAddr());
                pkt = new Packet(req, cmd, flit->getSize());
                // Data will be provided later via COPYBACKWRDATA flits.
                pkt->allocate();
                if (flit->getCacheResponding()) {
                    pkt->setCacheResponding();
                    pkt->setExpressSnoop();
                }
                if (flit->getResponderHadWritable()) {
                    pkt->setResponderHadWritable();
                }

                TxnMeta meta;
                meta.opcode = op;
                meta.addr = flit->getAddr();
                meta.size = flit->getSize();
                meta.srcId = flit->getSrcId();
                // meta.returnNid = flit->getReturnNid();
                // meta.returnTxnId = flit->getReturnTxnid();
                meta.txnId = flit->getTxnId();
                meta.dbid = txnId;
                meta.pkt = pkt;
                meta.dataBits.assign((flit->getSize() + 31) / 32, false);
                meta.req = std::make_shared<Request>(op, flit->getAddr(), flit->getSize());
                meta.cacheResponding = flit->getCacheResponding();
                meta.responderHadWritable = flit->getResponderHadWritable();
                meta.retireAfterXbarSend = flit->getCacheResponding();
                txnTable[txnId] = meta;
                DPRINTF(CHIL3Txn,
                    "txnTable insert key=%u reason=cpu_req_track opcode=%s addr=%#lx size=%u src=%u retTxn=%u size_now=%u\n",
                    txnId, CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op),
                    flit->getAddr(), flit->getSize(), flit->getSrcId(),
                    flit->getReturnTxnid(), static_cast<unsigned>(txnTable.size()));
                trackCacheReqTxn(pkt, flit->getAddr(), txnId);
                //although CLEANUNIQUE is not a read-type request, it has same ordering constraint as read,
                // so we treat it as write for tracking purpose.
                trackPendingRead(pkt->getAddr());
                if (hasPendingWrite(pkt->getAddr())) {
                    enqueueBlockedRead(pkt, txnId);
                } else {
                    dispatchReadToXbar(pkt, txnId);
                }
                return true;
            }
            case CHI_OP_TYPE::CHI_REQ_EVICT:{
                uint32_t txnId = allocateTxnId();
                if (txnId == TxnIDManager::InvalidTxnId) {
                    DPRINTF(CHIL3, "No free TxnID available for new evict request, opcode=%s addr=%#lx\n",
                            CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op), flit->getAddr());
                    return false;
                }
                // //first check if we can send back resp
                // FlitPtr comp = std::make_unique<Flit>();
                // comp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMP);
                // comp->setSrcId(_NodeID);
                // comp->setTgtId(flit->getSrcId());
                // comp->setTxnId(flit->getTxnId());
                // DPRINTF(CHIL3,
                //         "cpuSide send COMP src=%u tgt=%u txn=%u addr=%#lx\n",
                //         _NodeID, flit->getSrcId(), flit->getTxnId(), flit->getAddr());
                // if (!networkPort->send(comp)) {
                //     DPRINTF(CHIL3,
                //             "cpuSide send COMP blocked txn=%u\n",
                //             flit->getTxnId());
                //     releaseTxn(txnId);
                //     return false;
                // }
                PacketPtr pkt = nullptr;
                MemCmd cmd = mapChiReqToMemCmd(op);
                auto req = std::make_shared<gem5::Request>(
                    flit->getAddr(), flit->getSize(), Flags<uint64_t>(0),
                    0);
                req->setPaddr(flit->getAddr());
                pkt = new Packet(req, cmd, flit->getSize());
                pkt->allocate();
                if (flit->getCacheResponding()) {
                    pkt->setCacheResponding();
                    pkt->setExpressSnoop();
                }
                if (flit->getResponderHadWritable()) {
                    pkt->setResponderHadWritable();
                }
                //here we successful send the COMP, we can send the evict req to xbar,
                // if xbar is not available, we can retry later.
                TxnMeta meta;
                meta.opcode = op;
                meta.addr = flit->getAddr();
                meta.size = flit->getSize();
                meta.srcId = flit->getSrcId();
                // meta.returnNid = flit->getReturnNid();
                // meta.returnTxnId = flit->getReturnTxnid();
                meta.txnId = flit->getTxnId();
                meta.dbid = txnId;
                meta.pkt = pkt;
                meta.dataBits.assign((flit->getSize() + 31) / 32, false);
                meta.req = std::make_shared<Request>(op, flit->getAddr(), flit->getSize());
                meta.cacheResponding = flit->getCacheResponding();
                meta.responderHadWritable = flit->getResponderHadWritable();
                meta.retireAfterXbarSend = true;
                txnTable[txnId] = meta;
                DPRINTF(CHIL3Txn,
                    "txnTable insert key=%u reason=cpu_req_track opcode=%s addr=%#lx size=%u src=%u retTxn=%u size_now=%u\n",
                    txnId, CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op),
                    flit->getAddr(), flit->getSize(), flit->getSrcId(),
                    flit->getReturnTxnid(), static_cast<unsigned>(txnTable.size()));
                trackCacheReqTxn(pkt, flit->getAddr(), txnId);
                //although CHI_REQ_EVICT is not a read-type request, it has same ordering constraint as read,
                // so we treat it as write for tracking purpose.
                trackPendingRead(pkt->getAddr());
                if (hasPendingWrite(pkt->getAddr())) {
                    enqueueBlockedRead(pkt, txnId);
                } else {
                    dispatchReadToXbar(pkt, txnId);
                }
                return true;
            }
            default:
              panic("Unsupported REQ opcode %s",
                    CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op));
          }


      }
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP: {
          auto it = txnTable.find(flit->getTxnId());
          if (it == txnTable.end())
              panic("RSP for unknown txn %u", flit->getTxnId());
          if (flit->getOpcode() == CHI_OP_TYPE::CHI_RSP_COMPACK) {
                DPRINTF(CHIL3, "Txn %u completed via COMPACK\n", flit->getTxnId());
                completePendingRead(it->second.addr);
                eraseCacheReqTxn(it->second.pkt, it->second.addr,
                                 flit->getTxnId());
                    DPRINTF(CHIL3Txn,
                        "txnTable erase key=%u reason=cpu_rsp_compack size_before=%u\n",
                        flit->getTxnId(), static_cast<unsigned>(txnTable.size()));
                txnTable.erase(it);
                    DPRINTF(CHIL3Txn,
                        "txnTable size_after=%u\n",
                        static_cast<unsigned>(txnTable.size()));
                releaseTxn(flit->getTxnId());
              return true;
          }
          panic("Unsupported RSP opcode %s",
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()));
      }
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA: {
          if (flit->getOpcode() != CHI_OP_TYPE::CHI_DAT_COPYBACKWRDATA) {
              panic("Unsupported cpuSide DATA opcode %s",
                    CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()));
          }
          // For writeback, COPYBACKWRDATA txnId is DBID assigned by us.
          auto it = txnTable.find(flit->getTxnId());
          if (it == txnTable.end()) {
              panic("COPYBACKWRDATA for unknown DBID txn %u", flit->getTxnId());
          }
          if (!it->second.req) {
              panic("writeback txn %u missing Request", flit->getTxnId());
          }

          it->second.req->gatherDataFlit(flit);
          it->second.req->finishTransferdata(flit->getDataId());
          if (!it->second.req->dataTransferFinished()) {
              return true;
          }

          PacketPtr pkt = it->second.pkt;
          uint8_t *tmp = new uint8_t[pkt->getSize()];
          it->second.req->getData(tmp);
          pkt->setData(tmp);
          delete[] tmp;

          dispatchWriteToXbar(pkt, flit->getTxnId());
          return true;
      }
      default:
        panic("Unsupported channel type from cpuSide: %d",
              static_cast<int>(flit->get_Flit_Channel_Type()));
    }
}

bool
CHI_L3::handleMemSideFlit(FlitPtr &flit)
{
    DPRINTF(CHIL3, "memSide recv flit opcode=%s addr=%#lx txn=%u\n",
            CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()),
            flit->getAddr(), flit->getTxnId());
    switch (flit->getOpcode()) {
      case CHI_OP_TYPE::CHI_RSP_DBIDRESP: {
          auto it = txnTable.find(flit->getTxnId());
          if (it == txnTable.end()) {
              panic("DBIDRESP for unknown txn %u", flit->getTxnId());
          }

          // For WRITENOSNPFULL path from cache->DDR: do not forward to RN.
          // Instead, stream NCBWRDATACOMPACK to DDR one flit per cycle.
          if (it->second.opcode == CHI_OP_TYPE::CHI_REQ_WRITENOSNPFULL) {
              if (!it->second.req) {
                  panic("write req txn %u missing Request payload", flit->getTxnId());
              }
              PendingWriteData p;
              p.req = it->second.req;
              p.txnId = flit->getTxnId();
              p.ddrDbid = flit->getDbid();
              p.tgtId = SAM ? SAM->getTargetID(it->second.addr) : 0;
              writeDataQ.push_back(p);
              scheduleNetworkRetry(
                  writeDataSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
              return true;
          }

          FlitPtr rsp = std::make_unique<Flit>();
          rsp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMPDBIDRESP);
          rsp->setTxnId(it->second.returnTxnId);
          rsp->setDbid(flit->getDbid());
          rsp->setSrcId(_NodeID);
          rsp->setTgtId(it->second.srcId);
          DPRINTF(CHIL3,
                  "memSide->cpuSide send COMPDBIDRESP src=%u tgt=%u txn=%u dbid=%u(orig_txn=%u)\n",
                  _NodeID, it->second.srcId, it->second.returnTxnId,
                  flit->getDbid(), flit->getTxnId());
          if (!networkPort->send(rsp)) {
              warn("COMPDBIDRESP send failed txn=%u", flit->getTxnId());
              return false;
          }
          releaseTxn(flit->getTxnId());
          DPRINTF(CHIL3Txn,
              "txnTable erase key=%u reason=mem_dbidresp_forwarded size_before=%u\n",
              flit->getTxnId(), static_cast<unsigned>(txnTable.size()));
          txnTable.erase(it);
          DPRINTF(CHIL3Txn,
              "txnTable size_after=%u\n",
              static_cast<unsigned>(txnTable.size()));
          return true;
      }
      case CHI_OP_TYPE::CHI_DAT_COMPDATA: {
          auto it = txnTable.find(flit->getTxnId());
          if (it == txnTable.end()) {
              panic("COMPDATA for unknown txn %u", flit->getTxnId());
          }
          auto &meta = it->second;
          const unsigned idx = flit->getDataId();
          if (idx >= meta.dataBits.size()) {
              panic("COMPDATA dataId out of range idx=%u", idx);
          }
          meta.dataBits[idx] = true;
          if (!meta.req) {
              panic("txn %u missing req for COMPDATA gather", flit->getTxnId());
          }
          meta.req->gatherDataFlit(flit);
          meta.req->finishTransferdata(idx);
          if (!meta.req->dataTransferFinished()) {
              return true;
          }

          PacketPtr resp = meta.pkt;
          resp->makeTimingResponse();
          uint8_t *tmp = new uint8_t[resp->getSize()];
          meta.req->getData(tmp);
          resp->setData(tmp);
          delete [] tmp;

          DPRINTF(CHIL3,
                  "memSide->cache send timing resp txn=%u addr=%#lx size=%u\n",
                  flit->getTxnId(), resp->getAddr(), resp->getSize());

          if (!innerCacheRespPort.sendTimingResp(resp)) {
              warn("cache resp send failed txn=%u", flit->getTxnId());
              return false;
          }

        //   cacheReqMap.erase(meta.pkt);
          downstreamMap.erase(meta.pkt);
          if (meta.opcode == CHI_OP_TYPE::CHI_REQ_READNOSNP) {
              completeDdrRead(meta.addr);
          }
          releaseTxn(flit->getTxnId());
          DPRINTF(CHIL3Txn,
              "txnTable erase key=%u reason=mem_compdata_done size_before=%u\n",
              flit->getTxnId(), static_cast<unsigned>(txnTable.size()));
          txnTable.erase(it);
          DPRINTF(CHIL3Txn,
              "txnTable size_after=%u\n",
              static_cast<unsigned>(txnTable.size()));
          return true;
      }
      default:
          panic("Unsupported memSide opcode %s",
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()));
    }
}

bool
CHI_L3::handleXBarCpuTimingReq(PacketPtr pkt)
{
    DPRINTF(CHIL3, "xbar->CHI_L3 timing req addr=%#lx size=%u cmd=%s\n",
            pkt->getAddr(), pkt->getSize(), pkt->cmd.toString());
    uint32_t txnId = TxnIDManager::InvalidTxnId;
    if (!popCacheReqTxn(pkt, pkt->getAddr(), txnId)) {
        panic("xbar resp pkt not tracked for addr=%#lx", pkt->getAddr());
    }
    auto metaIt = txnTable.find(txnId);
    if (metaIt == txnTable.end()) {
        panic("txn %u missing meta for xbar resp", txnId);
    }
    // Handle CLEANUNIQUE completion: send CHI_RSP_COMP and wait COMPACK
    if (metaIt->second.req->getOpcode() == CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE) {
        assert(pkt->cmd == MemCmd::UpgradeResp);
        pendingCompRspQ.push_back(txnId);
        scheduleNetworkRetry(
            compRspSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
        return true;
    }
    // Queue data flits: one flit per tick, Request manages dataId
    PendingData pd;
    metaIt->second.req->setData(pkt);
    pd.req = metaIt->second.req;
    pd.txnId = metaIt->second.txnId;
    pd.srcId = _NodeID;
    pd.tgtId = metaIt->second.srcId;
    pd.HomeNid = _NodeID;
    // pd.returnNid = metaIt->second.returnNid;
    // pd.returnTxnId = metaIt->second.returnTxnId;
    pd.dbid = metaIt->second.dbid;
    dataQ.push_back(pd);
    scheduleNetworkRetry(
        dataSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
    return true;
}

bool
CHI_L3::isDdrReadCmd(const PacketPtr pkt) const
{
    return pkt->cmd == MemCmd(MemCmd::ReadCleanReq) ||
           pkt->cmd == MemCmd(MemCmd::ReadExReq) ||
           pkt->cmd == MemCmd(MemCmd::ReadSharedReq);
}

bool
CHI_L3::isDdrWriteCmd(const PacketPtr pkt) const
{
    return pkt->cmd == MemCmd(MemCmd::WritebackClean) ||
           pkt->cmd == MemCmd(MemCmd::WritebackDirty);
}

void
CHI_L3::trackDdrReadStart(Addr addr)
{
    const Addr blk = blockAddr(addr);
    ddrReadInFlightCount[blk]++;
    DPRINTF(CHIL3,
            "track ddr read start blk=%#lx count=%u\n",
            blk, ddrReadInFlightCount[blk]);
}

void
CHI_L3::completeDdrRead(Addr addr)
{
    const Addr blk = blockAddr(addr);
    auto it = ddrReadInFlightCount.find(blk);
    if (it == ddrReadInFlightCount.end()) {
        return;
    }
    if (it->second > 0) {
        it->second--;
    }
    DPRINTF(CHIL3,
            "complete ddr read blk=%#lx count=%u\n",
            blk, it->second);
    if (it->second == 0) {
        ddrReadInFlightCount.erase(it);
    }
}

bool
CHI_L3::hasDdrReadInFlight(Addr addr) const
{
    const Addr blk = blockAddr(addr);
    auto it = ddrReadInFlightCount.find(blk);
    return it != ddrReadInFlightCount.end() && it->second > 0;
}

void
CHI_L3::trackDdrWriteStart(Addr addr)
{
    const Addr blk = blockAddr(addr);
    ddrWriteInFlightCount[blk]++;
    DPRINTF(CHIL3,
            "track ddr write start blk=%#lx count=%u\n",
            blk, ddrWriteInFlightCount[blk]);
}

void
CHI_L3::completeDdrWrite(Addr addr)
{
    const Addr blk = blockAddr(addr);
    auto it = ddrWriteInFlightCount.find(blk);
    if (it == ddrWriteInFlightCount.end()) {
        return;
    }
    if (it->second > 0) {
        it->second--;
    }
    DPRINTF(CHIL3,
            "complete ddr write blk=%#lx count=%u\n",
            blk, it->second);
    if (it->second == 0) {
        ddrWriteInFlightCount.erase(it);
        wakeBlockedDdrReads(blk);
    }
}

bool
CHI_L3::hasDdrWriteInFlight(Addr addr) const
{
    const Addr blk = blockAddr(addr);
    auto it = ddrWriteInFlightCount.find(blk);
    return it != ddrWriteInFlightCount.end() && it->second > 0;
}

void
CHI_L3::enqueueBlockedDdrRead(PacketPtr pkt, uint32_t txnId, CHI_OP_TYPE chiOp)
{
    const Addr blk = blockAddr(pkt->getAddr());
    blockedDdrReadByAddr[blk].push_back({pkt, txnId, chiOp});
    DPRINTF(CHIL3,
            "block ddr read txn=%u addr=%#lx blk=%#lx pending_ddr_write=%u blocked_reads=%u\n",
            txnId,
            pkt->getAddr(),
            blk,
            ddrWriteInFlightCount[blk],
            static_cast<unsigned>(blockedDdrReadByAddr[blk].size()));
}

void
CHI_L3::wakeBlockedDdrReads(Addr addr)
{
    const Addr blk = blockAddr(addr);
    auto it = blockedDdrReadByAddr.find(blk);
    if (it == blockedDdrReadByAddr.end()) {
        return;
    }

    auto &queue = it->second;
    DPRINTF(CHIL3,
            "wake blocked ddr reads blk=%#lx count=%u\n",
            blk, static_cast<unsigned>(queue.size()));

    while (!queue.empty()) {
        PendingDdrReq pending = queue.front();
        queue.pop_front();
        if (!sendReadToDdr(pending.pkt, pending.txnId, pending.chiOp)) {
            DPRINTF(CHIL3,
                    "wake ddr read blocked txn=%u addr=%#lx, enqueue retry\n",
                    pending.txnId,
                    pending.pkt->getAddr());
            enqueuePendingDdr(pending.pkt, pending.txnId, pending.chiOp);
        }
    }

    blockedDdrReadByAddr.erase(it);
}

bool
CHI_L3::handleCacheMemTimingResp(PacketPtr pkt)
{
    DPRINTF(CHIL3, "cache->CHI_L3 timing resp addr=%#lx size=%u cmd=%s\n",
            pkt->getAddr(), pkt->getSize(), pkt->cmd.toString());
    if (pkt->cmd == MemCmd::UpgradeReq) {
        DPRINTF(CHIL3,
                "UpgradeReq received for addr=%#lx size=%u, send UpgradeResp to cache\n",
                pkt->getAddr(), pkt->getSize());
        if (pkt->cacheResponding()&&pkt->isExpressSnoop()){
            DPRINTF(CHIL3,
                    "ingnore L3's snoop\n");
            return true;
        }
        pkt->makeTimingResponse();
        if (!innerCacheRespPort.sendTimingResp(pkt)) {
            DPRINTF(CHIL3,
                    "UpgradeResp send blocked for addr=%#lx\n",
                    pkt->getAddr());
            return false;
        }
        return true;
    }
    if (isDdrReadCmd(pkt) || isDdrWriteCmd(pkt)) {
        // Downstream miss path: allocate txn and send CHI REQ toward DDR
        CHI_OP_TYPE chiOp = pkt->isRead() ? CHI_OP_TYPE::CHI_REQ_READNOSNP
                                        : CHI_OP_TYPE::CHI_REQ_WRITENOSNPFULL;

        const Addr blk = blockAddr(pkt->getAddr());
        if (pkt->isWrite()) {
            assert(!hasDdrReadInFlight(blk) &&
                    "L3->DDR write must not overlap same-address in-flight read");
            assert(!hasDdrWriteInFlight(blk) &&
                    "L3->DDR write must not overlap same-address in-flight write");
        } else {
            assert(!hasDdrReadInFlight(blk) &&
                    "L3->DDR read must not overlap same-address in-flight read");
        }
        //here we assume L3 to ddr has no limited txns
        uint32_t txnId = txnIdMgr.getUntrackID();
        if (txnId == TxnIDManager::InvalidTxnId) {
            DPRINTF(CHIL3, "No free TxnID available for new downstream request, opcode=%s addr=%#lx\n",
                    CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(chiOp), pkt->getAddr());
            panic("No free TxnID available for new downstream request");
        }
        TxnMeta meta;
        meta.opcode = chiOp;
        meta.addr = pkt->getAddr();
        meta.size = pkt->getSize();
        meta.srcId = _NodeID;
        meta.returnNid = 0;
        meta.returnTxnId = 0;
        meta.dbid = txnId;
        meta.pkt = pkt;
        meta.dataBits.assign((pkt->getSize() + 31) / 32, false);
        meta.req = std::make_shared<Request>(chiOp, pkt->getAddr(), pkt->getSize());
        meta.req->setTransactionId(txnId);
        if (pkt->isWrite() && pkt->hasData()) {
            meta.req->setData(pkt);
        }

        // If this pkt is from a tracked CPU request, preserve original CHI txn/src.
        const uint32_t upTxnId = peekCacheReqTxn(pkt, pkt->getAddr());
        if (upTxnId != TxnIDManager::InvalidTxnId) {
            auto upTxnIt = txnTable.find(upTxnId);
            if (upTxnIt != txnTable.end()) {
                meta.srcId = upTxnIt->second.srcId;
                meta.returnTxnId = upTxnId; // original cpu txn id
                meta.returnNid = upTxnIt->second.returnNid;
            }
        }

        txnTable[txnId] = meta;
        DPRINTF(CHIL3Txn,
            "txnTable insert key=%u reason=cache_miss_downstream opcode=%s addr=%#lx size=%u src=%u retTxn=%u size_now=%u\n",
            txnId, CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(chiOp), pkt->getAddr(),
            pkt->getSize(), meta.srcId, meta.returnTxnId,
            static_cast<unsigned>(txnTable.size()));
        downstreamMap[pkt] = txnId;

        if (pkt->isRead()) {
            trackDdrReadStart(pkt->getAddr());
            if (hasDdrWriteInFlight(pkt->getAddr())) {
                enqueueBlockedDdrRead(pkt, txnId, chiOp);
                return true;
            }
            if (!sendReadToDdr(pkt, txnId, chiOp)) {
                DPRINTF(CHIL3,
                        "REQ->DDR blocked, enqueue retry txn=%u addr=%#lx\n",
                        txnId, pkt->getAddr());
                enqueuePendingDdr(pkt, txnId, chiOp);
            }
            return true;
        }
        if (pkt->isWrite()) {
            trackDdrWriteStart(pkt->getAddr());
            if (!sendWriteToDdr(pkt, txnId, chiOp)) {
                DPRINTF(CHIL3,
                        "REQ->DDR blocked, enqueue retry txn=%u addr=%#lx\n",
                        txnId, pkt->getAddr());
                enqueuePendingDdr(pkt, txnId, chiOp);
            }
            return true;
        }

    }
    if (pkt->cmd==MemCmd(MemCmd::CleanEvict)){
        DPRINTF(CHIL3, "CleanEvict received for addr=%#lx size=%u, send COMP to cache\n",
                pkt->getAddr(), pkt->getSize());
        return true;
    }
    panic("Unsupported downstream pkt cmd %s", pkt->cmd.toString());
}

MemCmd
CHI_L3::mapChiReqToMemCmd(CHI_OP_TYPE op) const
{
        switch (op) {
            case CHI_OP_TYPE::CHI_REQ_READUNIQUE:
                return MemCmd::ReadExReq;
            case CHI_OP_TYPE::CHI_REQ_READSHARED:
                return MemCmd::ReadSharedReq;
            case CHI_OP_TYPE::CHI_REQ_READCLEAN:
                return MemCmd::ReadCleanReq;
            case CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL:
                return MemCmd::WritebackDirty;
            case CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL:
                return MemCmd::WritebackDirty;
            case CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE:
                return MemCmd::UpgradeReq;
            case CHI_OP_TYPE::CHI_REQ_EVICT:
                return MemCmd::CleanEvict;
            default:
                panic("Unsupported CHI req opcode %d", static_cast<int>(op));
        }
}

CHI_OP_TYPE
CHI_L3::mapMemCmdToChiReq(const PacketPtr pkt) const
{
    switch (pkt->cmd.responseCommand()) {
        case MemCmd::ReadResp:
        case MemCmd::ReadRespWithInvalidate:
            return CHI_OP_TYPE::CHI_REQ_READSHARED;
        case MemCmd::ReadExResp:
            return CHI_OP_TYPE::CHI_REQ_READUNIQUE;
        default:
            panic("Unsupported mem cmd resp %s", pkt->cmd.toString());
    }
}

uint32_t
CHI_L3::allocateTxnId()
{
    const int id = txnIdMgr.getID();
    if (id < 0) {
        DPRINTF(CHIL3, "No free TxnID available\n");
        return TxnIDManager::InvalidTxnId;
    }
    DPRINTF(CHIL3Txn, "outstanding txn num %u\n", static_cast<unsigned>(txnTable.size()));
    return static_cast<uint32_t>(id);
}

void
CHI_L3::releaseTxn(uint32_t txnId)
{
        DPRINTF(CHIL3Txn,
            "txnId release id=%u\n",
            txnId);
        txnIdMgr.releaseID(txnId);
}

bool
CHI_L3::sendPktToXbar(PacketPtr pkt)
{
    DPRINTF(CHIL3,
        "send pkt to xbar addr=%#lx size=%u cmd=%s\n",
        pkt->getAddr(), pkt->getSize(), pkt->cmd.toString());
    const bool ok = innerCacheReqPort.sendTimingReq(pkt);
    DPRINTF(CHIL3,
        "send pkt to xbar %s addr=%#lx\n",
        ok ? "success" : "blocked", pkt->getAddr());
    return ok;
}

bool
CHI_L3::sendReadToDdr(PacketPtr pkt, uint32_t txnId, CHI_OP_TYPE chiOp)
{
    FlitPtr f = std::make_unique<Flit>();
    f->setOpcode(chiOp);
    f->setAddr(pkt->getAddr());
    f->setSize(pkt->getSize());
    f->setTxnId(txnId);
    f->setTgtId(SAM ? SAM->getTargetID(pkt->getAddr()) : 0);
    f->setSrcId(_NodeID);
    f->setReturnNid(_NodeID);
    f->setReturnTxnid(txnId);
        DPRINTF(CHIL3,
            "send REQ->DDR opcode=%s src=%u tgt=%u txn=%u retTxn=%u addr=%#lx size=%u\n",
            CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(chiOp), _NodeID,
            SAM ? SAM->getTargetID(pkt->getAddr()) : 0, txnId, txnId,
            pkt->getAddr(), pkt->getSize());
        const bool ok = networkPort->send(f);
        DPRINTF(CHIL3, "send REQ->DDR %s txn=%u addr=%#lx\n",
            ok ? "success" : "blocked", txnId, pkt->getAddr());
        return ok;
}

bool
CHI_L3::sendWriteToDdr(PacketPtr pkt, uint32_t txnId, CHI_OP_TYPE chiOp)
{
    FlitPtr f = std::make_unique<Flit>();
    f->setOpcode(chiOp);
    f->setAddr(pkt->getAddr());
    f->setSize(pkt->getSize());
    f->setTxnId(txnId);
    f->setTgtId(SAM ? SAM->getTargetID(pkt->getAddr()) : 0);
    f->setSrcId(_NodeID);
    f->setReturnNid(_NodeID);
    f->setReturnTxnid(txnId);
        DPRINTF(CHIL3,
            "send REQ->DDR opcode=%s src=%u tgt=%u txn=%u retTxn=%u addr=%#lx size=%u\n",
            CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(chiOp), _NodeID,
            SAM ? SAM->getTargetID(pkt->getAddr()) : 0, txnId, txnId,
            pkt->getAddr(), pkt->getSize());
        const bool ok = networkPort->send(f);
        DPRINTF(CHIL3, "send REQ->DDR %s txn=%u addr=%#lx\n",
            ok ? "success" : "blocked", txnId, pkt->getAddr());
        return ok;
}

void
CHI_L3::scheduleNetworkRetry(EventFunctionWrapper &event,
                             Flit::CHI_CHN_TYPE channel)
{
    if (event.scheduled()) {
        return;
    }
    if (networkPort->isChannelBlockedByCredit(channel)) {
        return;
    }
    schedule(event, clockEdge(Cycles(1)));
}

void
CHI_L3::handleCreditUnblock(Flit::CHI_CHN_TYPE channel)
{
    switch (channel) {
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
        if (!pendingDdrQ.empty()) {
            scheduleNetworkRetry(
                pendingDdrSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ);
        }
        return;
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
        if (!pendingCompRspQ.empty()) {
            scheduleNetworkRetry(
                compRspSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
        }
        return;
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
        if (!dataQ.empty()) {
            scheduleNetworkRetry(
                dataSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
        }
        if (!writeDataQ.empty()) {
            scheduleNetworkRetry(
                writeDataSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
        }
        return;
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_NUM:
        return;
    }
}

void
CHI_L3::drainDataQueue()
{
    if (dataQ.empty()) {
        return;
    }

    PendingData &pd = dataQ.front();
    if (!pd.req) {
        dataQ.pop_front();
        if (!dataQ.empty()) {
            scheduleNetworkRetry(
                dataSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
        }
        return;
    }

    const uint32_t dataId = pd.req->generateWriteDataID();
    FlitPtr dat = std::make_unique<Flit>();
    dat->setOpcode(CHI_OP_TYPE::CHI_DAT_COMPDATA);
    dat->setDataId(dataId);
    dat->setSize(pd.req->getSize());
    dat->setAddr(pd.req->getAddr());
    dat->setData(pd.req);
    dat->setTgtId(pd.tgtId);
    dat->setSrcId(pd.srcId);
    dat->setTxnId(pd.txnId);
    // dat->setReturnNid(pd.returnNid);
    // dat->setReturnTxnid(pd.returnTxnId);
    dat->setHomeNid(pd.HomeNid);
    dat->setDbid(pd.dbid);

    DPRINTF(CHIL3,
            "send DAT->cpu opcode=%s txn=%u dataId=%u addr=%#lx size=%u tgt=%u\n",
            CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(CHI_OP_TYPE::CHI_DAT_COMPDATA),
            pd.txnId, dataId, pd.req->getAddr(), pd.req->getSize(), pd.tgtId);

    if (!networkPort->send(dat)) {
        DPRINTF(CHIL3,
                "send DAT->cpu blocked txn=%u dataId=%u\n",
                pd.txnId, dataId);
        scheduleNetworkRetry(
            dataSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
        return;
    }

    pd.req->finishTransferdata(dataId);

    if (pd.req->dataTransferFinished()) {
        // FlitPtr comp = std::make_unique<Flit>();
        // comp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMPACK);
        // comp->setTxnId(pd.txnId);
        // comp->setSrcId(pd.srcId);
        // comp->setTgtId(pd.tgtId);
        // comp->setReturnNid(pd.returnNid);
        // comp->setReturnTxnid(pd.returnTxnId);
        // comp->setDbid(pd.dbid);
        // DPRINTF(CHIL3,
        //         "send RSP->cpu COMPACK txn=%u tgt=%u\n",
        //         pd.txnId, pd.tgtId);
        // if (!cpuSidePort->send(comp)) {
        //     DPRINTF(CHIL3,
        //             "send RSP->cpu COMPACK blocked txn=%u\n",
        //             pd.txnId);
        //     if (!dataSendEvent.scheduled()) {
        //         schedule(dataSendEvent, clockEdge(Cycles(1)));
        //     }
        //     return;
        // }

        // auto metaIt = txnTable.find(pd.txnId);
        // if (metaIt != txnTable.end()) {
        //     cacheReqMap.erase(metaIt->second.pkt);
        //         DPRINTF(CHIL3Txn,
        //             "txnTable erase key=%u reason=dataq_compack_done size_before=%zu\n",
        //             pd.txnId, txnTable.size());
        //     txnTable.erase(metaIt);
        //         DPRINTF(CHIL3Txn,
        //             "txnTable size_after=%zu\n",
        //             txnTable.size());
        // }
        // releaseTxn(pd.txnId);
        dataQ.pop_front();
    }

    if (!dataQ.empty()) {
        scheduleNetworkRetry(
            dataSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
    }
}

void
CHI_L3::drainCompRspQueue()
{
    if (pendingCompRspQ.empty()) {
        return;
    }

    const uint32_t txnKey = pendingCompRspQ.front();
    auto it = txnTable.find(txnKey);
    if (it == txnTable.end()) {
        panic("pending COMP txn %u not found in txnTable", txnKey);
        pendingCompRspQ.pop_front();
        if (!pendingCompRspQ.empty()) {
            scheduleNetworkRetry(
                compRspSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
        }
        return;
    }

    auto &meta = it->second;
    FlitPtr comp = std::make_unique<Flit>();
    comp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMP);
    comp->setSrcId(_NodeID);
    comp->setTgtId(meta.srcId);
    comp->setTxnId(meta.txnId);
    comp->setDbid(txnKey);

    DPRINTF(CHIL3,
            "send RSP->cpu COMP src=%u tgt=%u txn=%u dbid=%u addr=%#lx\n",
            _NodeID, meta.srcId, meta.txnId, txnKey, meta.addr);

    if (!networkPort->send(comp)) {
        DPRINTF(CHIL3,
                "send RSP->cpu COMP blocked txn=%u dbid=%u\n",
                meta.txnId, txnKey);
        scheduleNetworkRetry(
            compRspSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
        return;
    }
    if (meta.opcode == CHI_OP_TYPE::CHI_REQ_EVICT){
        DPRINTF(CHIL3, "Txn %u completed via COMPACK\n", txnKey);
        completePendingRead(it->second.addr);
        eraseCacheReqTxn(it->second.pkt, it->second.addr, txnKey);
            DPRINTF(CHIL3Txn,
                "txnTable erase key=%u reason=cpu_rsp_compack size_before=%u\n",
                txnKey, static_cast<unsigned>(txnTable.size()));
        txnTable.erase(it);
            DPRINTF(CHIL3Txn,
                "txnTable size_after=%u\n",
                static_cast<unsigned>(txnTable.size()));
        releaseTxn(txnKey);
    }

    pendingCompRspQ.pop_front();
    if (!pendingCompRspQ.empty()) {
        scheduleNetworkRetry(
            compRspSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
    }
}

void
CHI_L3::drainWriteDataQueue()
{
    if (writeDataQ.empty()) {
        return;
    }

    PendingWriteData &pd = writeDataQ.front();
    if (!pd.req) {
        writeDataQ.pop_front();
        if (!writeDataQ.empty()) {
            scheduleNetworkRetry(
                writeDataSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
        }
        return;
    }

    const uint32_t dataId = pd.req->generateWriteDataID();
    FlitPtr dat = std::make_unique<Flit>();
    dat->setOpcode(CHI_OP_TYPE::CHI_DAT_NCBWRDATACOMPACK);
    dat->setDataId(dataId);
    dat->setCcid(0);
    dat->setSize(pd.req->getSize());
    dat->setAddr(pd.req->getAddr());
    dat->setData(pd.req);
    dat->setTgtId(pd.tgtId);
    dat->setSrcId(_NodeID);
    dat->setTxnId(pd.ddrDbid);

    DPRINTF(CHIL3,
            "send DAT->DDR opcode=%s ddrDbid=%u dataId=%u addr=%#lx size=%u tgt=%u\n",
            CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(CHI_OP_TYPE::CHI_DAT_NCBWRDATACOMPACK),
            pd.ddrDbid, dataId, pd.req->getAddr(), pd.req->getSize(), pd.tgtId);

    if (!networkPort->send(dat)) {
        DPRINTF(CHIL3,
                "send DAT->DDR blocked ddrDbid=%u dataId=%u\n",
                pd.ddrDbid, dataId);
        scheduleNetworkRetry(
            writeDataSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
        return;
    }

    pd.req->finishTransferdata(dataId);
    if (pd.req->dataTransferFinished()) {
        auto it = txnTable.find(pd.txnId);
        if (it != txnTable.end()) {
            const Addr write_addr = it->second.addr;
            downstreamMap.erase(it->second.pkt);
                DPRINTF(CHIL3Txn,
                    "txnTable erase key=%u reason=write_data_done size_before=%zu\n",
                    pd.txnId, txnTable.size());
            txnTable.erase(it);
                DPRINTF(CHIL3Txn,
                    "txnTable size_after=%zu\n",
                    txnTable.size());
            completeDdrWrite(write_addr);
        }
        releaseTxn(pd.txnId);
        writeDataQ.pop_front();
    }

    if (!writeDataQ.empty()) {
        scheduleNetworkRetry(
            writeDataSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
    }
}

void
CHI_L3::drainPendingXbarQueue()
{
    if (pendingXbarQ.empty()) {
        return;
    }

    PendingXbarReq &p = pendingXbarQ.front();
    if (!sendPktToXbar(p.pkt)) {
        xbarRetryPending = true;
        DPRINTF(CHIL3,
                "pending xbar retry wait addr=%#lx queue=%u\n",
                p.pkt->getAddr(), static_cast<unsigned>(pendingXbarQ.size()));
        return;
    }

    xbarRetryPending = false;

    if (p.cleanupTxn) {
        auto it = txnTable.find(p.txnId);
        assert(it->second.opcode == CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL ||
                it->second.opcode == CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL ||
                it->second.opcode == CHI_OP_TYPE::CHI_REQ_EVICT ||
                it->second.opcode == CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE);
        if (it->second.opcode == CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL ||
                it->second.opcode == CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL ){
            //consider write is done
            if (it != txnTable.end()) {
                const Addr write_addr = it->second.addr;
                eraseCacheReqTxn(it->second.pkt, write_addr, p.txnId);
                    DPRINTF(CHIL3Txn,
                            "txnTable erase key=%u reason=pending_xbar_cleanup size_before=%u\n",
                            p.txnId, static_cast<unsigned>(txnTable.size()));
                txnTable.erase(it);
                    DPRINTF(CHIL3Txn,
                            "txnTable size_after=%u\n",
                            static_cast<unsigned>(txnTable.size()));
                completePendingWrite(write_addr);
            }
            releaseTxn(p.txnId);
        }else if (it->second.opcode == CHI_OP_TYPE::CHI_REQ_EVICT ||
                    it->second.opcode == CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE){
            DPRINTF(CHIL3,
                    "Due to UpgradeReq is set iscacheResponding send CompREP after \
                    dispatched CLEANUNIQUE to xbar, enqueue comp rsp txn=%u addr=%#lx\n",
                    it->first, (it->second.pkt)->getAddr());
            pendingCompRspQ.push_back(it->first);
            scheduleNetworkRetry(
                compRspSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);

        }else {
            panic("Unsupported opcode for pending xbar cleanup: %s",
                    CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(it->second.opcode));
        }

    } else {
        auto it = txnTable.find(p.txnId);
        assert(p.txnId != TxnIDManager::InvalidTxnId);
        assert(it->second.opcode == CHI_OP_TYPE::CHI_REQ_READCLEAN ||
                it->second.opcode == CHI_OP_TYPE::CHI_REQ_READSHARED ||
                it->second.opcode == CHI_OP_TYPE::CHI_REQ_READUNIQUE);
        assert(txnIdMgr.isUsed(p.txnId));
        // completePendingRead(p.pkt->getAddr());
    }
    pendingXbarQ.pop_front();

    if (!pendingXbarQ.empty() && !xbarRetryPending && !pendingXbarSendEvent.scheduled()) {
        schedule(pendingXbarSendEvent, clockEdge(Cycles(1)));
    }
}

void
CHI_L3::drainPendingDdrQueue()
{
    if (pendingDdrQ.empty()) {
        return;
    }

    PendingDdrReq &p = pendingDdrQ.front();
    bool ok = false;
    if (p.pkt->isRead()) {
        ok = sendReadToDdr(p.pkt, p.txnId, p.chiOp);
    } else if (p.pkt->isWrite()) {
        ok = sendWriteToDdr(p.pkt, p.txnId, p.chiOp);
    } else {
        panic("pending DDR req has unsupported cmd %s", p.pkt->cmd.toString());
    }

    if (!ok) {
        DPRINTF(CHIL3,
                "pending REQ->DDR still blocked txn=%u addr=%#lx queue=%u\n",
                p.txnId, p.pkt->getAddr(), static_cast<unsigned>(pendingDdrQ.size()));
        scheduleNetworkRetry(
            pendingDdrSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ);
        return;
    }

    DPRINTF(CHIL3,
            "pending REQ->DDR sent txn=%u addr=%#lx\n",
            p.txnId, p.pkt->getAddr());
    pendingDdrQ.pop_front();

    if (!pendingDdrQ.empty()) {
        scheduleNetworkRetry(
            pendingDdrSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ);
    }
}

void
CHI_L3::enqueuePendingDdr(PacketPtr pkt, uint32_t txnId, CHI_OP_TYPE chiOp)
{
    DPRINTF(CHIL3,
            "enqueue pending ddr pkt addr=%#lx txn=%u opcode=%s queue=%u\n",
            pkt->getAddr(), txnId, CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(chiOp),
            static_cast<unsigned>(pendingDdrQ.size() + 1));
    pendingDdrQ.push_back({pkt, txnId, chiOp});
    scheduleNetworkRetry(
        pendingDdrSendEvent, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ);
}

void
CHI_L3::enqueuePendingXbar(PacketPtr pkt, bool cleanupTxn, uint32_t txnId)
{
    DPRINTF(CHIL3,
            "enqueue pending xbar pkt addr=%#lx cleanup=%d txn=%u queue=%u\n",
            pkt->getAddr(), cleanupTxn, txnId,
            static_cast<unsigned>(pendingXbarQ.size() + 1));
    pendingXbarQ.push_back({pkt, cleanupTxn, txnId});

    if (!xbarRetryPending && !pendingXbarSendEvent.scheduled()) {
        schedule(pendingXbarSendEvent, clockEdge(Cycles(1)));
    }
}

void
CHI_L3::enqueuePendingCacheMemReq(PacketPtr pkt)
{
    pendingCacheMemReqQ.push_back(pkt);
    DPRINTF(CHIL3,
            "enqueue pending cache-mem req addr=%#lx cmd=%s queue=%u\n",
            pkt->getAddr(), pkt->cmd.toString(),
            static_cast<unsigned>(pendingCacheMemReqQ.size()));
    if (!pendingCacheMemReqSendEvent.scheduled()) {
        schedule(pendingCacheMemReqSendEvent, clockEdge(Cycles(1)));
    }
}

void
CHI_L3::drainPendingCacheMemReqQueue()
{
    if (pendingCacheMemReqQ.empty()) {
        return;
    }

    PacketPtr pkt = pendingCacheMemReqQ.front();
    if (!handleCacheMemTimingResp(pkt)) {
        DPRINTF(CHIL3,
                "pending cache-mem req still blocked addr=%#lx cmd=%s queue=%u\n",
                pkt->getAddr(), pkt->cmd.toString(),
                static_cast<unsigned>(pendingCacheMemReqQ.size()));
        if (!pendingCacheMemReqSendEvent.scheduled()) {
            schedule(pendingCacheMemReqSendEvent, clockEdge(Cycles(1)));
        }
        return;
    }

    pendingCacheMemReqQ.pop_front();
    DPRINTF(CHIL3,
            "pending cache-mem req sent addr=%#lx remaining=%u\n",
            pkt->getAddr(),
            static_cast<unsigned>(pendingCacheMemReqQ.size()));

    if (!pendingCacheMemReqQ.empty() && !pendingCacheMemReqSendEvent.scheduled()) {
        schedule(pendingCacheMemReqSendEvent, clockEdge(Cycles(1)));
    }
}

bool
CHI_L3::InnerCacheReqPort::recvTimingResp(PacketPtr pkt)
{
    return owner->handleXBarCpuTimingReq(pkt);
}

void
CHI_L3::InnerCacheReqPort::recvReqRetry()
{
    DPRINTF(CHIL3, "xbar cpu-side req retry\n");
    owner->xbarRetryPending = false;
    if (!owner->pendingXbarQ.empty() && !owner->pendingXbarSendEvent.scheduled()) {
        owner->schedule(owner->pendingXbarSendEvent, owner->clockEdge(Cycles(1)));
    }
}

void
CHI_L3::InnerCacheReqPort::recvRangeChange()
{
    // ignore
}

void
CHI_L3::InnerCacheReqPort::recvTimingSnoopReq(PacketPtr pkt)
{
    panic("CHI_L3 does not implement snoops");
}

void
CHI_L3::InnerCacheReqPort::recvFunctionalSnoop(PacketPtr pkt)
{
    panic("CHI_L3 does not implement functional snoops");
}

Tick
CHI_L3::InnerCacheReqPort::recvAtomicSnoop(PacketPtr pkt)
{
    panic("CHI_L3 does not implement atomic snoops");
}

bool
CHI_L3::InnerCacheRespPort::recvTimingReq(PacketPtr pkt)
{
    // Keep xbar-side handshake non-blocking: accept request and defer retry
    // internally if CHI_L3 cannot make forward progress this cycle.
    if (!owner->pendingCacheMemReqQ.empty()) {
        owner->enqueuePendingCacheMemReq(pkt);
        return true;
    }

    if (!owner->handleCacheMemTimingResp(pkt)) {
        owner->enqueuePendingCacheMemReq(pkt);
    }
    return true;
}

Tick
CHI_L3::InnerCacheRespPort::recvAtomic(PacketPtr pkt)
{
    panic("CHI_L3 does not support atomic accesses");
}

void
CHI_L3::InnerCacheRespPort::recvFunctional(PacketPtr pkt)
{
    panic("CHI_L3 does not support functional accesses");
}

void
CHI_L3::InnerCacheRespPort::recvRespRetry()
{
    DPRINTF(CHIL3, "cache mem-side resp retry\n");
    if (!owner->pendingCacheMemReqQ.empty() &&
        !owner->pendingCacheMemReqSendEvent.scheduled()) {
        owner->schedule(owner->pendingCacheMemReqSendEvent,
                        owner->clockEdge(Cycles(1)));
    }
}

AddrRangeList
CHI_L3::InnerCacheRespPort::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(RangeSize(0, MaxAddr));
    return ranges;
}

} // namespace xsCHI
} // namespace gem5
