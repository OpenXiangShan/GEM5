#ifndef __CPU_O3_PERFCCT_HH__
#define __CPU_O3_PERFCCT_HH__

#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
    BlockStartTick,
    ReqCreateTick,
    L1Miss,
    L2Miss,
    L3Miss,
    L1MissTick,
    L1SendTick,
    L1RespRecvTick,
    L1ReturnTick,
    L2ReturnTick,
    L3ReturnTick,
    DataReadyTick,
    EffL2Miss,
    EffL3Miss,
    EffL2MissTick,
    EffL2SendTick,
    EffL2RespRecvTick,
    EffL2ReturnTick,
    EffL3MissTick,
    EffL3SendTick,
    EffL3RespRecvTick,
    EffL3ReturnTick,
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
    uint64_t value;

    bool isload;
    Addr vaddr;
    Addr paddr;
    Tick lastReplay;
    Tick blockStartTick;
    bool blockStartTickValid;
    Tick reqCreateTick;
    bool l1Miss;
    bool l2Miss;
    bool l3Miss;
    Tick l1MissTick;
    Tick l1SendTick;
    Tick l1RespRecvTick;
    Tick l1ReturnTick;
    Tick l2ReturnTick;
    Tick l3ReturnTick;
    Tick dataReadyTick;
    bool effL2Miss;
    bool effL3Miss;
    Tick effL2MissTick;
    Tick effL2SendTick;
    Tick effL2RespRecvTick;
    Tick effL2ReturnTick;
    Tick effL3MissTick;
    Tick effL3SendTick;
    Tick effL3RespRecvTick;
    Tick effL3ReturnTick;
    std::vector<std::pair<uint8_t, Tick>> replayEvents;
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
    std::string ld_replay_insert_cmd;

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
