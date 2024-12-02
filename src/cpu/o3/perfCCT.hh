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


class InstMeta
{
    friend class PerfCCT;
    InstSeqNum sn;
    std::vector<uint64_t> posTick;
    std::string disasm;
    Addr pc;
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

    std::vector<InstMeta> metas;

    std::stringstream ss;

    InstMeta* getMeta(InstSeqNum sn);

  public:
    PerfCCT(bool enable, ArchDBer* db);

    void createMeta(const DynInstPtr inst);

    void updateInstPos(InstSeqNum sn, const PerfRecord pos);

    // void updateInstMeta

    void commitMeta(InstSeqNum sn);
};


}
}


#endif
