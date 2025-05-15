
#include "mem/ruby/structures/SharingManager.hh"

namespace gem5
{

namespace ruby
{

SharingManager::~SharingManager() = default;

SharingManager::SharingManager(const Params &p) : SimObject(p), sharingTable()
{
    // TODO
    controller = p.controller;
    downstream_hnfs = p.downstream_hnfs;
    downstream_snfs = p.downstream_snfs;
    if (p.xid < 0 || p.yid < 0) {
        fatal("SharingManager: xid and yid must be set\n");
    }
    id = p.machineid;
    coordinate = p.coordinate;
}

void
SharingManager::init()
{
    for (auto cntrl : downstreamHNFs) {
        for (const AddrRange &range : cntrl->getAddrRanges()) {
            if (hnfMap.intersects(range) != hnfMap.end()) {
                fatal("HNF %s range %s overlaps others\n", cntrl->name(), range.to_string());
            }
            hnfMap.insert(range, cntrl->getMachineID());
        }
        slaveCoordinateMap.insert({cntrl->getMachineID(), cntrl->getSharingManager()->getCoordinate()});
    }
    for (auto cntrl : downstreamSNFs) {
        for (const AddrRange &range : cntrl->getAddrRanges()) {
            if (snfMap.intersects(range) != snfMap.end()) {
                fatal("SNF %s range %s overlaps others\n", cntrl->name(), range.to_string());
            }
            snfMap.insert(range, cntrl->getMachineID());
        }
        slaveCoordinateMap.insert({cntrl->getMachineID(), cntrl->getSharingManager()->getCoordinate()});
    }
    for (auto cntrl : rnfs) {
        Coordinate c = cntrl->getSharingManager()->getCoordinate();
        MachineID id = cntrl->getMachineID();
        RNFCoordinateMap.insert({c, id});
    }
}

/*
 * Processor indicates that a given address range is shared within mesh row or column
 */
void
SharingManager::setSharing(Addr addr, Addr size, SharingDirection direction)
{
    sharingTable.push_back({addr, size, direction});
}

void
SharingManager::clearSharing(Addr addr)
{
    for (auto it = sharingTable.begin(); it != sharing.end(); it++) {
        if (it->addr == addr) {
            sharing.erase(it);
            break;
        }
    }
}

/* check if a given address is shared */
SharingDirection
SharingManager::getSharingDirection(Addr addr)
{
    for (auto it = sharingTable.begin(); it != sharing.end(); it++) {
        if (it->addr == addr) {
            return it->direction;
        }
    }
    return SharingDirection::NONE;
}

SharingManager::Coordinate
SharingManager::Coordinate::NaiveNeighbour(Coordinate slave, SharingDirection direction) const
{
    int next_xid = xid;
    int next_yid = yid;
    if (direction == SharingDirection::ROW) {
        // same yid, minimize xid diff
        if (slave.xid == xid) {
            return Coordinate(xid, yid);
        } else {
            next_xid = slave.xid > xid ? xid + 1 : xid - 1;
        }
    } else if (direction == SharingDirection::COL) {
        // same xid, minimize yid diff
        if (slave.yid == yid) {
            return Coordinate(xid, yid);
        } else {
            next_yid = slave.yid > yid ? yid + 1 : yid - 1;
        }
    }
    // TODO Handle special case when

    return Coordinate(next_xid, next_yid);
}


MachineID
SharingManager::getDestination(Addr addr, SharingDirection direction) const
{
    auto slave_mapping = snfMap;
    std::map<AddrRange, MachineID>::iterator c = slave_mapping.contains(addr);
    Coordinate slave_coord;
    if (c != slave_mapping.end()) {
        slave_coord = slaveCoordinateMap.at(c->second);
        DPRINTF(SharingManager, "Addr %lx current id  coord %s, slave id %d coord %s\n",
            addr, id, coordinate, c->second, slave_coord)
    } else {
        fatal("SharingManager: No downstream destination for %s\n", addr);
    }

    // Neighbouring grid coordinate, may not have RN there
    Coordinate naive_neighbour = coordinate.NaiveNeighbour(slave_coord, direction);

    if (naive_neighbour == slave_coord) {
        // RN and slave on same grid, dest is slave
        DPRINTF(SharingManager, "Naive neighour is slave\n");
        return c->second;
    }

    auto rnf_it = RNFCoordinateMap.find(naive_neighbour);
    if (rnf_it == RNFCoordinateMap.end()) {
        // Naive neighbour does not map to RN,
        // means current RN is closest to slave
        DPRINTF(SharingManager, "Current RN is closest to slave\n");
        return c->second;
    }

    DPRINF(SharingManager, "Next target is neighbouring RN id %d coor %s\n",
        rnf_it->second, naive_neighbour);
    return rnf_it->second;
}

// Get previous holder of target
// MachineID
// SharingManager::getBackwardTgt(Addr addr, SharingDirection direction) const
// {

// }

}
}
