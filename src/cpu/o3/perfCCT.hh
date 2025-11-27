#ifndef __CPU_O3_PERFCCT_HH__
#define __CPU_O3_PERFCCT_HH__

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
    const int MaxMetas = 1500;  // same as MaxNum of DynInst
    bool enableCCT;
    ArchDBer* archdb;
    std::string sql_insert_cmd;
    std::string ld_insert_cmd;

    uint64_t id = 0;
    std::vector<InstMeta> metas;

    std::stringstream ss;

    InstMeta* getMeta(InstSeqNum sn);

  public:
    PerfCCT(bool enable, ArchDBer* db);

    void createMeta(const DynInstPtr inst);

    void updateInstPos(InstSeqNum sn, const PerfRecord pos);

    void updateInstMeta(InstSeqNum sn, const InstDetail detail, const uint64_t val);

    void commitMeta(InstSeqNum sn);
};


}
}


#endif
