#ifndef __CPU_O3_PERFCCT_HH__
#define __CPU_O3_PERFCCT_HH__

#include <map>
#include <string>

#include "base/types.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "enums/PerfRecord.hh"
#include "sim/arch_db.hh"

namespace gem5
{
namespace o3
{

enum class InstDetail
{
    Result,
    VAddress,
    PAddress,
    LastReplay,
    ReplayStr,
};

enum ReplayReason
{
    TT_CacheMiss,
    TT_TLBMiss,
    TT_BankConflict,
    TT_Nuke,
    TT_DcacheStall,
    TT_RARReplay,
    TT_RAWReplay,
    TT_OtherReplay,
    TT_NumReplay
};

static char ReplayReasonStr[] = {
    'C',
    'T',
    'B',
    'N',
    'S',
    'R',
    'W',
    'O'
};

class InstMeta
{

    friend class PerfCCT;
    InstSeqNum sn;
    std::vector<Tick> posTick;
    std::string disasm;
    Addr pc;
    ThreadID tid;
    uint64_t value;

    bool isload;
    Addr vaddr;
    Addr paddr;
    Tick lastReplay;
    std::stringstream replayStr;
  public:

    void reset(const DynInstPtr inst);
};

// performanceCounter commitTrace
class PerfCCT
{
    const int MaxMetas = 3000;  // same as MaxNum of DynInst
    bool enableCCT;
    ArchDBer* archdb;
    std::string sql_insert_cmd;
    std::string ld_insert_cmd;

    uint64_t id = 0;
    std::vector<InstMeta> metas;

    std::stringstream ss;

    bool enableCausal;
    std::string cpuName;
    // Physical values retain their producer after commit, until reallocation.
    std::map<unsigned, std::pair<ThreadID, InstSeqNum>> regProducers;
    struct CommitSpan
    {
        Tick start, end;
        InstSeqNum head, blocker;
        std::string reason;
        unsigned committed;
        uint64_t samples;
    };
    std::map<ThreadID, CommitSpan> commitSpans;
    void writeSpan(ThreadID tid, const CommitSpan &span,
                   const char *end_kind);
    void finishCausal();

    InstMeta* getMeta(InstSeqNum sn);

  public:
    PerfCCT(bool enable, ArchDBer* db, const std::string &cpu_name,
            Tick clock_period);

    bool enabled() const { return enableCausal; }
    void loadEvent(const DynInstPtr &inst, const char *event,
                   const char *detail = "", InstSeqNum related = 0,
                   uint64_t mask = 0);
    void instEvent(const DynInstPtr &inst, const char *event,
                   const char *detail = "", InstSeqNum related = 0,
                   uint64_t mask = 0);
    void renameSource(const DynInstPtr &inst, unsigned index, bool ready);
    void renameDestination(const DynInstPtr &inst, unsigned index, bool alias);
    void contextIdentity(unsigned requestor, ThreadID tid, ContextID context);
    void resetProducers(ThreadID tid);
    void squashInst(const DynInst &inst);
    void fuseInst(const DynInstPtr &removed, const DynInstPtr &fused);
    void commitState(ThreadID tid, const DynInstPtr &head,
                     const DynInstPtr &blocker, const char *reason,
                     unsigned committed, Tick cycle_ticks);
    void fetchPartialTransfer(const DynInstPtr &anchor,
                              unsigned empty_slots, bool topdown_eligible);

    void createMeta(const DynInstPtr inst);

    void updateInstPos(InstSeqNum sn, const PerfRecord pos);

    void updateInstMeta(InstSeqNum sn, const InstDetail detail, const uint64_t val);

    void commitMeta(InstSeqNum sn);
};


}
}


#endif
