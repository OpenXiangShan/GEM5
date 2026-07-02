#include "cpu/o3/perfCCT.hh"

#include "cpu/o3/dyn_inst.hh"

namespace gem5
{
namespace o3
{
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
    memType = inst->isAtomic() ? 'A'
              : inst->isLoad() ? 'L'
              : inst->isStore() ? 'S' : '\0';
    vaddr = 0;
    paddr = 0;
    lastReplay = 0;
    replayStr.str(std::string());
    replayTicks.str(std::string());
    executeTicks.str(std::string());

    stallReason = "NoStall";
    stallCycles = 0;
    secondaryReason = "NoStall";
    stallSpans = "";
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

        // SquashedLifeTimeTrace has the identical column set as
        // LifeTimeCommitTrace, so reuse the same column list, just a new table.
        ss << "INSERT INTO SquashedLifeTimeTrace(";
        ss << PerfRecordStrings[0];
        for (int i=1; i < (int)PerfRecord::Num_PerfRecord; i++) {
            ss << "," << PerfRecordStrings[i];
        }
        ss << ") VALUES(";
        squash_insert_cmd = ss.str();
        ss.str(std::string());

        ld_insert_cmd = "insert into LoadLifeTimeCommitTrace(ID, VAddress, "
                        "PAddress, LastReplay, ReplayStr, ReplayTicks, "
                        "ExecuteTicks) Values (";
    }
}

// Serialize the shared LifeTimeCommitTrace column tuple (everything after the
// "INSERT ... VALUES(" prefix and before the closing ")") for `meta` into `s`.
void
PerfCCT::dumpMetaRow(std::stringstream& s, const InstMeta* meta)
{
    s << meta->posTick[0];
    for (auto it = meta->posTick.begin() + 1; it != meta->posTick.end(); it++) {
        s << "," << *it;
    }
    s << "," << (meta->value & 0x0fffffffffffffffllu);
    s << ",\'" << meta->disasm << "\'";
    s << "," << (meta->pc & 0x0fffffffffffffffllu);
    s << ",\'" << meta->stallReason << "\'";
    s << "," << meta->stallCycles;
    s << ",\'" << meta->secondaryReason << "\'";
    s << ",\'" << meta->stallSpans << "\'";
    const char mt[2] = {meta->memType, '\0'};
    s << ",\'" << (meta->memType ? mt : "") << "\'";
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
    meta->posTick.at((int)pos) = curTick();
    // accumulate one AtFU tick per pass (a replayed load re-executes)
    if (pos == PerfRecord::AtFU) {
        if (meta->executeTicks.tellp() != std::streampos(0)) {
            meta->executeTicks << ' ';
        }
        meta->executeTicks << curTick();
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
        if (meta->replayTicks.tellp() != std::streampos(0)) {
            meta->replayTicks << ' ';
        }
        meta->replayTicks << val;
        break;
    }
    case InstDetail::ReplayStr:{
        assert(val < sizeof(LdStReplayCharStr));
        meta->replayStr << LdStReplayCharStr[val];
        break;
    }
    case InstDetail::StallReason:{
        meta->stallReason = stallReasonToString((int)val);
        break;
    }
    case InstDetail::StallCycles:{
        meta->stallCycles = val;
        break;
    }
    case InstDetail::SecondaryReason:{
        meta->secondaryReason = stallReasonToString((int)val);
        break;
    }
    default:
        break;
    }
}

void
PerfCCT::updateInstMetaStr(InstSeqNum sn, const InstDetail detail,
                           const std::string& val)
{
    if (!enableCCT) [[likely]] {
        return;
    }
    auto meta = getMeta(sn);
    switch (detail) {
    case InstDetail::StallSpans:{
        meta->stallSpans = val;
        break;
    }
    default:
        break;
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
    dumpMetaRow(ss, meta);
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
        ss << '\'' << meta->replayStr.str() << '\'';
        ss << ",\'" << meta->replayTicks.str() << '\'';
        ss << ",\'" << meta->executeTicks.str() << '\'';
        ss << ");";
        archdb->execmd(ss.str());
        ss.str(std::string());
    }
}

void
PerfCCT::squashMeta(InstSeqNum sn)
{
    if (!enableCCT) [[likely]] {
        return;
    }
    auto meta = getMeta(sn);
    // only dump if it still belongs to this inst and it
    // actually started (was fetched).
    if (meta->sn != sn) {
        return;
    }
    if (meta->posTick.at((int)PerfRecord::AtFetch) == 0) {
        return;
    }
    // Reuse AtCommit as the squash tick: it bounds every structure the inst was
    // still occupying when it got squashed (those have no later-stage tick).
    meta->posTick.at((int)PerfRecord::AtCommit) = curTick();
    ss << squash_insert_cmd;
    dumpMetaRow(ss, meta);
    ss << ");";
    archdb->execmd(ss.str());
    ss.str(std::string());
}

}
}
