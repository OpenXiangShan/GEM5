#ifndef __ES_METADATA_HH__
#define __ES_METADATA_HH__

#include <string>
#include <string_view>

#include "cpu/valuepred/valuepred_metadata.hh"

namespace gem5
{

namespace valuepred
{

class ESUpdateInfoExt : public VPUpdateInfoExtension
{
  public:
    ESUpdateInfoExt(bool is_load_inst, uint64_t inflight_time, std::string disassembly)
        : isLoadInst(is_load_inst),
          inflightTime(inflight_time),
          disas(std::move(disassembly))
    {
    }

    bool isLoadInst = false;
    uint64_t inflightTime = 0;
    std::string disas;
};

}

}


#endif
