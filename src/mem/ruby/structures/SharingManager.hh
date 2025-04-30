#ifndef __MEM_RUBY_STRUCTURES_SHARING_MANAGER_HH__
#define __MEM_RUBY_STRUCTURES_SHARING_MANAGER_HH__

#include <vector>

#include "base/addr_range.hh"
#include "base/addr_range_map.hh"
#include "mem/cache/base.hh"
#include "mem/ruby/common/MachineID.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"
#include "params/SharingManager.hh"
#include "sim/sim_object.hh"

namespace gem5 {

namespace ruby {

class AbstractController;

class SharingManager : public SimObject
{
    public:
    typedef SharingManagerParams Params;
    SharingManager(const Params &p);
    ~SharingManager();
    void init();

    void setSharing(Addr addr, Addr size, int direction);
    void clearSharing(Addr addr);
    bool checkSharing(Addr addr);

    MachineID getDestination(Addr addr) const;

    struct SharingEntry
    {
        Addr addr;
        Addr size;
        int direction;
    };

    private:
    std::vector<SharingEntry> sharing;
    std::vector<AbstractController*> downstream_hnfs;
    std::vector<AbstractController*> downstream_snfs;
    AddrRangeMap<MachineID, 3> hnfMap;
    AddrRangeMap<MachineID, 3> snfMap;
};
}
}

#endif
