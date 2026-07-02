#ifndef __CPU_O3_PERFCCT_HH__
#define __CPU_O3_PERFCCT_HH__

#include <string>

#include "base/types.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/replay_events.hh"
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
    StallReason,
    StallCycles,
    SecondaryReason,
    StallSpans,
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


static const char LdStReplayCharStr[] = {
    'T',  // TLBMissReplay
    'C',  // CacheMissReplay
    'E',  // RescheduleReplay
    'F',  // STLFReplay
    'M',  // MdpAddrReplay
    'N',  // NukeReplay
    'K',  // CacheBlockedReplay
    'B',  // BankConflictReplay
    'R',  // RARReplay
    'W',  // RAWReplay
    'A',  // MshrAliasFailReplay
    'H',  // HitInWriteBufferReplay
    'G',  // MshrArbFailReplay
};

class InstMeta
{

    friend class PerfCCT;
    InstSeqNum sn;
    std::vector<Tick> posTick;
    std::string disasm;
    Addr pc;
    uint64_t value;

    bool isload;
    // 'L' load / 'S' store / 'A' atomic / '\0' non-mem (for LQ/SQ occupancy)
    char memType;
    Addr vaddr;
    Addr paddr;
    Tick lastReplay;
    std::stringstream replayStr;
    // Absolute tick of every replay event
    std::stringstream replayTicks;
    // Absolute tick of every AtFU (execution start); one per pass for replayed
    // loads. Lets tooling draw the per-pass IssueQ->Execute sawtooth.
    std::stringstream executeTicks;

    // Per-instruction stall attribution (mirrors dyn_inst stallProfile).
    std::string stallReason;
    uint64_t stallCycles;
    std::string secondaryReason;
    std::string stallSpans;
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
    std::string squash_insert_cmd;

    uint64_t id = 0;
    std::vector<InstMeta> metas;

    std::stringstream ss;

    InstMeta* getMeta(InstSeqNum sn);

    static void dumpMetaRow(std::stringstream& s, const InstMeta* meta);

  public:
    PerfCCT(bool enable, ArchDBer* db);

    void createMeta(const DynInstPtr inst);

    void updateInstPos(InstSeqNum sn, const PerfRecord pos);

    void updateInstMeta(InstSeqNum sn, const InstDetail detail, const uint64_t val);

    void updateInstMetaStr(InstSeqNum sn, const InstDetail detail,
                           const std::string& val);

    void commitMeta(InstSeqNum sn);

    void squashMeta(InstSeqNum sn);
};


}
}


#endif
