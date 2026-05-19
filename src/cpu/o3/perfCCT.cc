#include "cpu/o3/perfCCT.hh"

#include <algorithm>

#include "cpu/o3/dyn_inst.hh"

namespace gem5
{
namespace o3
{

namespace
{

void
updateEarliestTick(Tick &dst, Tick tick)
{
    if (dst == 0 || tick < dst) {
        dst = tick;
    }
}

void
updateLatestTick(Tick &dst, Tick tick)
{
    dst = std::max(dst, tick);
}

} // namespace

void
InstMeta::reset(const DynInstPtr inst)
{
    this->sn = inst->seqNum;
    posTick.clear();
    posTick.resize((int)PerfRecord::AtCommit + 1, 0);
    disasm = inst->staticInst->disassemble(inst->pcState().instAddr());
    pc = inst->pcState().instAddr();
    value = 0;

    isload = inst->isLoad();
    vaddr = 0;
    paddr = 0;
    lastReplay = 0;
    blockStartTick = 0;
    blockStartTickValid = false;
    reqCreateTick = 0;
    l1Miss = false;
    l2Miss = false;
    l3Miss = false;
    l1MissTick = 0;
    l1SendTick = 0;
    l1RespRecvTick = 0;
    l1ReturnTick = 0;
    l2ReturnTick = 0;
    l3ReturnTick = 0;
    dataReadyTick = 0;
    effL2Miss = false;
    effL3Miss = false;
    effL2MissTick = 0;
    effL2SendTick = 0;
    effL2RespRecvTick = 0;
    effL2ReturnTick = 0;
    effL3MissTick = 0;
    effL3SendTick = 0;
    effL3RespRecvTick = 0;
    effL3ReturnTick = 0;
    replayEvents.clear();
    replayStr.str(std::string());
    replayStr.clear();
}


PerfCCT::PerfCCT(bool enable, ArchDBer* db) : enableCCT(enable), archdb(db)
{
    if (enableCCT) {
        metas.resize(MaxMetas);

        ss << "INSERT INTO LifeTimeCommitTrace(";
        ss << PerfRecordStrings[0];
        for (int i=1; i < (int)PerfRecord::Num_PerfRecord; i++) {
            ss << "," << PerfRecordStrings[i];
        }
        ss << ") VALUES(";
        sql_insert_cmd = ss.str();
        ss.str(std::string());

        ld_insert_cmd =
            "insert into LoadLifeTimeCommitTrace("
            "ID, VAddress, PAddress, LastReplay, ReplayStr, "
            "L1Miss, L2Miss, L3Miss, L1ReturnTick, L2ReturnTick, "
            "L3ReturnTick, DataReadyTick, EffL2Miss, EffL3Miss, "
            "EffL2ReturnTick, EffL3ReturnTick, ReqCreateTick, "
            "L1MissTick, L1SendTick, L1RespRecvTick, EffL2MissTick, "
            "EffL2SendTick, EffL2RespRecvTick, EffL3MissTick, "
            "EffL3SendTick, EffL3RespRecvTick) Values (";
        ld_replay_insert_cmd = "insert into LoadReplayTrace"
                               "(ID, ReplayIdx, ReplayReason, ReplayTick, "
                               "BlockStartTick, Extra0, Extra1) Values (";
    }
}

InstMeta*
PerfCCT::getMeta(InstSeqNum sn)
{
    auto& meta = metas[sn % MaxMetas];
    return &meta;
}

void
PerfCCT::createMeta(const DynInstPtr inst)
{
    if (!enableCCT) [[likely]] {
        return;
    }
    auto& old = metas[inst->seqNum % MaxMetas];
    old.reset(inst);
}

void
PerfCCT::updateInstPos(InstSeqNum sn, const PerfRecord pos)
{
    if (!enableCCT) [[likely]] {
        return;
    }
    auto meta = getMeta(sn);
    if (meta->posTick.at((int)pos) == 0) {
        meta->posTick.at((int)pos) = curTick();
    }
}

void
PerfCCT::updateInstMeta(InstSeqNum sn, const InstDetail detail, const uint64_t val)
{
    if (!enableCCT) [[likely]] {
        return;
    }
    auto meta = getMeta(sn);
    switch (detail) {
    case InstDetail::Result: {
        meta->value = val;
        break;
    }
    case InstDetail::VAddress: {
        meta->vaddr = val;
        break;
    }
    case InstDetail::PAddress: {
        meta->paddr = val;
        break;
    }
    case InstDetail::LastReplay:{
        meta->lastReplay = val;
        break;
    }
    case InstDetail::ReplayStr:{
        assert(val < TT_NumReplay);
        meta->replayStr << ReplayReasonStr[val];
        meta->replayEvents.emplace_back(static_cast<uint8_t>(val), curTick());
        break;
    }
    case InstDetail::BlockStartTick:{
        if (!meta->blockStartTickValid) {
            meta->blockStartTick = val;
            meta->blockStartTickValid = true;
        }
        break;
    }
    case InstDetail::ReqCreateTick: {
        updateEarliestTick(meta->reqCreateTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::L1Miss: {
        meta->l1Miss = val != 0;
        break;
    }
    case InstDetail::L2Miss: {
        meta->l2Miss = val != 0;
        break;
    }
    case InstDetail::L3Miss: {
        meta->l3Miss = val != 0;
        break;
    }
    case InstDetail::L1MissTick: {
        updateEarliestTick(meta->l1MissTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::L1SendTick: {
        updateLatestTick(meta->l1SendTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::L1RespRecvTick: {
        updateLatestTick(meta->l1RespRecvTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::L1ReturnTick: {
        updateLatestTick(meta->l1ReturnTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::L2ReturnTick: {
        updateLatestTick(meta->l2ReturnTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::L3ReturnTick: {
        updateLatestTick(meta->l3ReturnTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::DataReadyTick: {
        updateLatestTick(meta->dataReadyTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::EffL2Miss: {
        meta->effL2Miss = val != 0;
        break;
    }
    case InstDetail::EffL3Miss: {
        meta->effL3Miss = val != 0;
        break;
    }
    case InstDetail::EffL2MissTick: {
        updateEarliestTick(meta->effL2MissTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::EffL2SendTick: {
        updateLatestTick(meta->effL2SendTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::EffL2RespRecvTick: {
        updateLatestTick(meta->effL2RespRecvTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::EffL2ReturnTick: {
        updateLatestTick(meta->effL2ReturnTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::EffL3MissTick: {
        updateEarliestTick(meta->effL3MissTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::EffL3SendTick: {
        updateLatestTick(meta->effL3SendTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::EffL3RespRecvTick: {
        updateLatestTick(meta->effL3RespRecvTick, static_cast<Tick>(val));
        break;
    }
    case InstDetail::EffL3ReturnTick: {
        updateLatestTick(meta->effL3ReturnTick, static_cast<Tick>(val));
        break;
    }
    }
}

void
PerfCCT::commitMeta(InstSeqNum sn)
{
    if (!enableCCT) [[likely]] {
        return;
    }
    auto meta = getMeta(sn);
    ss << sql_insert_cmd;
    // dump counter first
    ss << meta->posTick[0];
    for (auto it = meta->posTick.begin() + 1; it != meta->posTick.end(); it++) {
        ss << "," << *it;
    }
    ss << "," << (meta->value & 0x0fffffffffffffffllu);
    ss << ",\'" << meta->disasm << "\'";
    ss << "," << (meta->pc & 0x0fffffffffffffffllu);
    ss << ");";
    archdb->execmd(ss.str());
    ss.str(std::string());

    id++;
    if (meta->isload) {
        ss << ld_insert_cmd;
        ss << id << ',';
        ss << meta->vaddr << ',';
        ss << meta->paddr << ',';
        ss << meta->lastReplay << ',';
        ss << '\'' << meta->replayStr.str() << '\'' << ',';
        ss << meta->l1Miss << ',';
        ss << meta->l2Miss << ',';
        ss << meta->l3Miss << ',';
        ss << meta->l1ReturnTick << ',';
        ss << meta->l2ReturnTick << ',';
        ss << meta->l3ReturnTick << ',';
        ss << meta->dataReadyTick << ',';
        ss << meta->effL2Miss << ',';
        ss << meta->effL3Miss << ',';
        ss << meta->effL2ReturnTick << ',';
        ss << meta->effL3ReturnTick << ',';
        ss << meta->reqCreateTick << ',';
        ss << meta->l1MissTick << ',';
        ss << meta->l1SendTick << ',';
        ss << meta->l1RespRecvTick << ',';
        ss << meta->effL2MissTick << ',';
        ss << meta->effL2SendTick << ',';
        ss << meta->effL2RespRecvTick << ',';
        ss << meta->effL3MissTick << ',';
        ss << meta->effL3SendTick << ',';
        ss << meta->effL3RespRecvTick;
        ss << ");";
        archdb->execmd(ss.str());
        ss.str(std::string());

        for (size_t i = 0; i < meta->replayEvents.size(); ++i) {
            const auto &event = meta->replayEvents[i];
            ss << ld_replay_insert_cmd;
            ss << id << ',';
            ss << i << ',';
            ss << '\'' << ReplayReasonStr[event.first] << '\'' << ',';
            ss << event.second << ',';
            ss << meta->blockStartTick << ',';
            ss << 0 << ',';
            ss << 0;
            ss << ");";
            archdb->execmd(ss.str());
            ss.str(std::string());
        }
    }
}

}
}
