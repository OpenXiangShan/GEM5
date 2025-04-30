
#include "mem/ruby/structures/SharingManager.hh"

namespace gem5
{

namespace ruby
{

SharingManager::~SharingManager() = default;

SharingManager::SharingManager(const Params &p) : SimObject(p) {
    // TODO
    downstream_hnfs = p.downstream_hnfs;
    downstream_snfs = p.downstream_snfs;
}

void
SharingManager::init() {
    for (auto cntrl : downstream_hnfs) {
        for (const AddrRange &range : cntrl->getAddrRanges()) {
            if (hnfMap.intersects(range) != hnfMap.end()) {
                fatal("HNF %s range %s overlaps others\n", cntrl->name(), range.to_string());
            }
            hnfMap.insert(range, cntrl->getMachineID());
        }
    }
    for (auto cntrl : downstream_snfs) {
        for (const AddrRange &range : cntrl->getAddrRanges()) {
            if (snfMap.intersects(range) != snfMap.end()) {
                fatal("SNF %s range %s overlaps others\n", cntrl->name(), range.to_string());
            }
            snfMap.insert(range, cntrl->getMachineID());
        }
    }
}

void SharingManager::setSharing(Addr addr, Addr size, int direction) {
    sharing.push_back({addr, size, direction});
}

void SharingManager::clearSharing(Addr addr) {
    for (auto it = sharing.begin(); it != sharing.end(); it++) {
        if (it->addr == addr) {
            sharing.erase(it);
            break;
        }
    }
}

bool SharingManager::checkSharing(Addr addr) {
    for (auto it = sharing.begin(); it != sharing.end(); it++) {
        if (it->addr == addr) {
            return it->direction;
        }
    }
    return 0;
}

MachineID
SharingManager::getDestination(Addr addr) const {
    auto mapping = snfMap;
    std::map<AddrRange, MachineID>::iterator c = mapping.contains(addr);
    if (c != mapping.end()) {
        return c->second;
    }
    fatal("SharingManager: No downstream destination for %s\n", addr);
}

}
}
