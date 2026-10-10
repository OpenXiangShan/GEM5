#include "cpu/o3/perfCCT.hh"

#include "cpu/o3/dyn_inst.hh"

namespace gem5
{
namespace o3
{
namespace
{
std::string
sqlQuote(const std::string &value)
{
    char *quoted = sqlite3_mprintf("%Q", value.c_str());
    std::string result(quoted);
    sqlite3_free(quoted);
    return result;
}
}

void
InstMeta::reset(const DynInstPtr inst)
{
    this->sn = inst->seqNum;
    posTick.clear();
    posTick.resize((int)PerfRecord::AtCommit + 1, 0);
    disasm = inst->staticInst->disassemble(inst->pcState().instAddr());
    pc = inst->pcState().instAddr();
    tid = inst->threadNumber;
    value = 0;

    isload = inst->isLoad();
    vaddr = 0;
    paddr = 0;
    lastReplay = 0;
    replayStr.str(std::string());
}


PerfCCT::PerfCCT(bool enable, ArchDBer* db, const std::string &cpu_name,
                 Tick clock_period)
    : enableCCT(enable), archdb(db),
      enableCausal(enable && db && db->dumpCausal), cpuName(cpu_name)
{
    if (enableCCT) {
        metas.resize(MaxMetas);

        ss << "INSERT INTO LifeTimeCommitTrace(";
        ss << PerfRecordStrings[0];
        for (int i=1; i < (int)PerfRecord::Num_PerfRecord; i++) {
            ss << "," << PerfRecordStrings[i];
        }
        ss << ",TID";
        ss << ") VALUES(";
        sql_insert_cmd = ss.str();
        ss.str(std::string());

        ld_insert_cmd = "insert into LoadLifeTimeCommitTrace(ID, VAddress, PAddress, LastReplay, ReplayStr) Values (";
    }
    if (enableCausal) {
        archdb->execmd(R"sql(
CREATE TABLE IF NOT EXISTS PerfCCTMeta(Key TEXT PRIMARY KEY, Value TEXT);
CREATE TABLE IF NOT EXISTS PerfCCTInst(
    Cpu TEXT, TID INTEGER, SeqNum INTEGER, PC INTEGER, Disasm TEXT,
    BornTick INTEGER, CommitID INTEGER, EndTick INTEGER, EndKind TEXT,
    PRIMARY KEY(Cpu,TID,SeqNum));
CREATE TABLE IF NOT EXISTS PerfCCTEvent(
    ID INTEGER PRIMARY KEY AUTOINCREMENT, Cpu TEXT, TID INTEGER,
    SeqNum INTEGER, Tick INTEGER, Attempt INTEGER, Event TEXT, Detail TEXT,
    RelatedSeq INTEGER, ReasonMask INTEGER);
CREATE INDEX IF NOT EXISTS PerfCCTEventInst
    ON PerfCCTEvent(Cpu,TID,SeqNum,Tick,ID);
CREATE TABLE IF NOT EXISTS PerfCCTCommitSpan(
    ID INTEGER PRIMARY KEY AUTOINCREMENT, Cpu TEXT, TID INTEGER,
    StartTick INTEGER, EndTick INTEGER, HeadSeq INTEGER, BlockerSeq INTEGER,
    Reason TEXT, Committed INTEGER, SampleCycles INTEGER, EndKind TEXT);
CREATE INDEX IF NOT EXISTS PerfCCTCommitWindow
    ON PerfCCTCommitSpan(Cpu,TID,StartTick,EndTick);
CREATE TABLE IF NOT EXISTS PerfCCTFetchTransfer(
    Tick INTEGER, Cpu TEXT, TID INTEGER, AnchorPC INTEGER,
    AnchorSeq INTEGER, FTQID INTEGER, EmptySlots INTEGER,
    TopdownEligible INTEGER);
CREATE INDEX IF NOT EXISTS PerfCCTFetchTransferWindow
    ON PerfCCTFetchTransfer(Cpu,TID,Tick,AnchorPC);
INSERT OR REPLACE INTO PerfCCTMeta VALUES ('schema_version','3');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('fetch_transfer_scope','Single-thread partial Decode transfers only; ' ||
     'anchor is the last sent instruction PC and FTQ ID, not the cause of empty slots');
INSERT OR REPLACE INTO PerfCCTMeta VALUES ('time_unit','tick');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('scope','Observed commit gates and load attempts; not a critical-path or CPI decomposition');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('capture','From CPU construction to simulation exit; independent of stats reset and dumpGlobal');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('attempt_semantics','Instruction attempt at observation; responses and hints are not request-attempt IDs');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('sample_cycles','Number of commit decisions observed; a trace_end span may include a final truncated sample');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('request_identity','Process-unique lazy Request object ID; packet copies sharing Request keep ID; ' ||
     'Request copies receive a new ID; no complete request birth/death tracing');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('writeback_scope','Scalar successful IEW writeback gate and scheduler non-spec wake observations; ' ||
     'not earliest bypass availability or complete readiness transitions; VP correction may wake before gate');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('scheduler_scope','IQ transition and decision observations; candidate readiness is speculative; ' ||
     'not complete operand availability or recoverable stall cycles');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('target_identity','Cache plus Target.order+1; add after insertion, remove before erase; ' ||
     'service is not CPU response arrival; replace retains TargetID and names old RequestID');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('cache_scope','L1 data cache only; resource identity is Cache plus allocation generation; ' ||
     'request identity requires Requestor and Context metadata; unknown seq is zero');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('cache_observation','Allocate occupancy is after allocation; release occupancy is before release; ' ||
     'blocked masks are before transitions; held credits are after transitions; ' ||
     'owner snapshots are candidate resource sets, not exclusive root causes');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('dependency_scope','Scalar physical value producers at rename, including ready sources; ' ||
     'aliases retain value producer; pinned and non-scalar writers unsupported; ' ||
     'readiness is not completion');
INSERT OR REPLACE INTO PerfCCTMeta VALUES
    ('identity','Fetched operation identity; split store address/data share SeqNum; ' ||
     'fused operations retain the first SeqNum');
)sql");
        archdb->execmd("INSERT OR REPLACE INTO PerfCCTMeta VALUES (" +
            sqlQuote("cpu." + cpuName + ".clock_period") + "," +
            sqlQuote(std::to_string(clock_period)) + ");");
        archdb->execmd("INSERT OR REPLACE INTO PerfCCTMeta VALUES (" +
            sqlQuote("cpu." + cpuName + ".start_tick") + "," +
            sqlQuote(std::to_string(curTick())) + ");");
        for (int bit = 0; bit < LdStReplayTypeCount; ++bit) {
            archdb->execmd("INSERT OR REPLACE INTO PerfCCTMeta VALUES (" +
                sqlQuote("replay_reason_bit." + std::to_string(bit)) + "," +
                sqlQuote(load_store_replay_event_str[bit]) + ");");
        }
        archdb->traceFinalizers.emplace_back([this] { finishCausal(); });
    }
}

InstMeta*
PerfCCT::getMeta(InstSeqNum sn)
{
    auto& meta = metas[sn % MaxMetas];
    return &meta;
}

void
PerfCCT::fetchPartialTransfer(const DynInstPtr &anchor,
                              unsigned empty_slots, bool topdown_eligible)
{
    if (!enableCausal) {
        return;
    }
    char *sql = sqlite3_mprintf(
        "INSERT INTO PerfCCTFetchTransfer VALUES(%lld,%Q,%d,%lld,%lld,%u,%u,%d);",
        static_cast<long long>(curTick()), cpuName.c_str(), anchor->threadNumber,
        static_cast<long long>(anchor->pcState().instAddr()),
        static_cast<long long>(anchor->seqNum), anchor->getFtqId(),
        empty_slots, topdown_eligible);
    archdb->execmd(sql);
    sqlite3_free(sql);
}

void
PerfCCT::createMeta(const DynInstPtr inst)
{
    if (!enableCCT) [[likely]] {
        return;
    }
    auto& old = metas[inst->seqNum % MaxMetas];
    old.reset(inst);
    if (enableCausal) {
        std::ostringstream cmd;
        cmd << "INSERT INTO PerfCCTInst VALUES (" << sqlQuote(cpuName)
            << ',' << inst->threadNumber << ',' << inst->seqNum << ','
            << static_cast<int64_t>(old.pc) << ',' << sqlQuote(old.disasm)
            << ',' << curTick() << ",NULL,NULL,'inflight');";
        archdb->execmd(cmd.str());
    }
}

void
PerfCCT::updateInstPos(InstSeqNum sn, const PerfRecord pos)
{
    if (!enableCCT) [[likely]] {
        return;
    }
    auto meta = getMeta(sn);
    meta->posTick.at((int)pos) = curTick();
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
    ss << "," << meta->tid;
    ss << ");";
    archdb->execmd(ss.str());
    ss.str(std::string());

    id = sqlite3_last_insert_rowid(archdb->mem_db);
    if (enableCausal) {
        std::ostringstream cmd;
        cmd << "UPDATE PerfCCTInst SET CommitID=" << id
            << ",EndTick=" << curTick() << ",EndKind='commit' WHERE Cpu="
            << sqlQuote(cpuName) << " AND TID=" << meta->tid
            << " AND SeqNum=" << sn << ';';
        archdb->execmd(cmd.str());
    }
    if (meta->isload) {
        ss << ld_insert_cmd;
        ss << id << ',';
        ss << meta->vaddr << ',';
        ss << meta->paddr << ',';
        ss << meta->lastReplay << ',';
        ss << '\'' << meta->replayStr.str() << '\'';
        ss << ");";
        archdb->execmd(ss.str());
        ss.str(std::string());
    }
}

void
PerfCCT::loadEvent(const DynInstPtr &inst, const char *event,
                   const char *detail, InstSeqNum related, uint64_t mask)
{
    if (!enableCausal || !inst->isLoad()) [[likely]] {
        return;
    }
    instEvent(inst, event, detail, related, mask);
}

void
PerfCCT::instEvent(const DynInstPtr &inst, const char *event,
                   const char *detail, InstSeqNum related, uint64_t mask)
{
    if (!enableCausal) [[likely]] {
        return;
    }
    std::ostringstream cmd;
    cmd << "INSERT INTO PerfCCTEvent(Cpu,TID,SeqNum,Tick,Attempt,Event,"
           "Detail,RelatedSeq,ReasonMask) VALUES ("
        << sqlQuote(cpuName) << ',' << inst->threadNumber << ','
        << inst->seqNum << ',' << curTick() << ',' << inst->perfCctAttempt
        << ',' << sqlQuote(event) << ',' << sqlQuote(detail) << ','
        << related << ',' << mask << ");";
    archdb->execmd(cmd.str());
}

void
PerfCCT::resetProducers(ThreadID tid)
{
    for (auto it = regProducers.begin(); it != regProducers.end();) {
        if (it->second.first == tid) {
            it = regProducers.erase(it);
        } else {
            ++it;
        }
    }
}

void
PerfCCT::contextIdentity(unsigned requestor, ThreadID tid, ContextID context)
{
    if (!enableCausal) {
        return;
    }
    const auto prefix = "cpu." + cpuName;
    archdb->execmd("INSERT OR REPLACE INTO PerfCCTMeta VALUES (" +
        sqlQuote(prefix + ".data_requestor") + "," +
        sqlQuote(std::to_string(requestor)) + ");");
    archdb->execmd("INSERT OR REPLACE INTO PerfCCTMeta VALUES (" +
        sqlQuote(prefix + ".context." + std::to_string(tid)) + "," +
        sqlQuote(std::to_string(context)) + ");");
}

void
PerfCCT::renameSource(const DynInstPtr &inst, unsigned index, bool ready)
{
    if (!enableCausal) [[likely]] {
        return;
    }
    const auto &arch = inst->srcRegIdx(index);
    const auto phys = inst->renamedSrcIdx(index);
    InstSeqNum producer = 0;
    const char *state = "unknown_initial_or_unsupported";
    if (arch.isZeroReg()) {
        state = "constant_zero";
    } else if (arch.is(IntRegClass) || arch.is(FloatRegClass)) {
        const auto found = regProducers.find(phys->flatIndex());
        if (found != regProducers.end() &&
            found->second.first == inst->threadNumber) {
            producer = found->second.second;
            state = "physical_value";
        }
    }
    std::ostringstream detail;
    detail << "src=" << index << ";phys=" << phys->flatIndex()
           << ";class=" << arch.className() << ";state=" << state
           << ";ready_at_rename=" << ready
           << ";folded=" << bool(inst->extRenamedSrcIdx(index).IEOper());
    if (inst->isSplitStoreAddr()) {
        detail << ";role=" << (index == 1 ? "store_data" : "store_address");
    }
    instEvent(inst, "dependency", detail.str().c_str(), producer);
}

void
PerfCCT::renameDestination(const DynInstPtr &inst, unsigned index, bool alias)
{
    if (!enableCausal || alias) {
        return;
    }
    const auto phys = inst->renamedDestIdx(index);
    const auto &arch = inst->destRegIdx(index);
    // Pinned and non-scalar values require multiple-writer tracking. A new
    // scalar allocation replaces any stale producer from physical reuse.
    if (arch.isZeroReg() ||
        !(arch.is(IntRegClass) || arch.is(FloatRegClass)) ||
        phys == inst->prevDestIdx(index).PhyReg() ||
        phys->getNumPinnedWritesToComplete() > 1) {
        regProducers.erase(phys->flatIndex());
        return;
    }
    regProducers[phys->flatIndex()] = {inst->threadNumber, inst->seqNum};
}

void
PerfCCT::squashInst(const DynInst &inst)
{
    if (!enableCausal) [[likely]] {
        return;
    }
    std::ostringstream cmd;
    cmd << "UPDATE PerfCCTInst SET EndTick=" << curTick()
        << ",EndKind='squash' WHERE Cpu=" << sqlQuote(cpuName)
        << " AND TID=" << inst.threadNumber << " AND SeqNum=" << inst.seqNum
        << " AND EndKind='inflight';";
    archdb->execmd(cmd.str());
    // Several pipeline stages can observe the same squash.
    if (inst.isLoad() && sqlite3_changes(archdb->mem_db)) {
        std::ostringstream event;
        event << "INSERT INTO PerfCCTEvent(Cpu,TID,SeqNum,Tick,Attempt,"
                 "Event,Detail,RelatedSeq,ReasonMask) VALUES ("
              << sqlQuote(cpuName) << ',' << inst.threadNumber << ','
              << inst.seqNum << ',' << curTick() << ',' << inst.perfCctAttempt
              << ",'squash','observed',0,0);";
        archdb->execmd(event.str());
    }
}

void
PerfCCT::fuseInst(const DynInstPtr &removed, const DynInstPtr &fused)
{
    if (!enableCausal) [[likely]] {
        return;
    }
    std::ostringstream cmd;
    cmd << "UPDATE PerfCCTInst SET EndTick=" << curTick()
        << ",EndKind='fused_into' WHERE Cpu=" << sqlQuote(cpuName)
        << " AND TID=" << removed->threadNumber
        << " AND SeqNum=" << removed->seqNum << ';'
        << "UPDATE PerfCCTInst SET Disasm="
        << sqlQuote(fused->staticInst->disassemble(fused->getPC()))
        << " WHERE Cpu=" << sqlQuote(cpuName)
        << " AND TID=" << fused->threadNumber
        << " AND SeqNum=" << fused->seqNum << ';'
        << "INSERT INTO PerfCCTEvent(Cpu,TID,SeqNum,Tick,Attempt,Event,"
           "Detail,RelatedSeq,ReasonMask) VALUES ("
        << sqlQuote(cpuName) << ',' << removed->threadNumber << ','
        << removed->seqNum << ',' << curTick()
        << ",0,'fused_into','decode'," << fused->seqNum << ",0);";
    archdb->execmd(cmd.str());
}

void
PerfCCT::writeSpan(ThreadID tid, const CommitSpan &span, const char *end_kind)
{
    std::ostringstream cmd;
    cmd << "INSERT INTO PerfCCTCommitSpan(Cpu,TID,StartTick,EndTick,"
           "HeadSeq,BlockerSeq,Reason,Committed,SampleCycles,EndKind) VALUES ("
        << sqlQuote(cpuName) << ',' << tid << ',' << span.start << ','
        << span.end << ',' << span.head << ',' << span.blocker << ','
        << sqlQuote(span.reason) << ',' << span.committed << ','
        << span.samples << ',' << sqlQuote(end_kind) << ");";
    archdb->execmd(cmd.str());
}

void
PerfCCT::commitState(ThreadID tid, const DynInstPtr &head,
                    const DynInstPtr &blocker, const char *reason,
                    unsigned committed, Tick cycle_ticks)
{
    if (!enableCausal) [[likely]] {
        return;
    }
    const InstSeqNum head_seq = head ? head->seqNum : 0;
    const InstSeqNum blocker_seq = blocker ? blocker->seqNum : 0;
    auto it = commitSpans.find(tid);
    if (it != commitSpans.end()) {
        auto &span = it->second;
        if (span.end == curTick() && span.head == head_seq &&
            span.blocker == blocker_seq && span.reason == reason &&
            span.committed == committed) {
            span.end += cycle_ticks;
            ++span.samples;
            return;
        }
        writeSpan(tid, span, span.end == curTick() ? "closed" : "gap");
    }
    commitSpans[tid] = {curTick(), curTick() + cycle_ticks, head_seq,
                       blocker_seq, reason, committed, 1};
}

void
PerfCCT::finishCausal()
{
    for (const auto &[tid, original] : commitSpans) {
        auto span = original;
        // The final simulated cycle may end at an instruction-count exit.
        span.end = std::min(span.end, curTick());
        writeSpan(tid, span, "trace_end");
    }
    commitSpans.clear();
    archdb->execmd("UPDATE PerfCCTInst SET EndKind='trace_end',EndTick=" +
        std::to_string(curTick()) + " WHERE Cpu=" + sqlQuote(cpuName) +
        " AND EndKind='inflight';");
    archdb->execmd("INSERT OR REPLACE INTO PerfCCTMeta VALUES (" +
        sqlQuote("cpu." + cpuName + ".end_tick") + "," +
        sqlQuote(std::to_string(curTick())) + ");");
}

}
}
