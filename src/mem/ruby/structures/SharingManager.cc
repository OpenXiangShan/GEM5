#include "mem/ruby/structures/SharingManager.hh"

#include "SharingManager.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"
#include "mem/ruby/structures/SharingManagerProxy.hh"

namespace gem5
{

namespace ruby
{

SharingManager::~SharingManager() = default;

SharingManager::SharingManager(const Params &p) : SimObject(p), sharingTable()
{
    /*controller = p.controller;*/
    downstreamHNFs = p.downstream_hnfs;
    downstreamSNFs = p.downstream_snfs;
    if (p.xid < 0 || p.yid < 0) {
        fatal("SharingManager: xid and yid must be set\n");
    }
    // id = MachineID(); // TODO need a way to get current machine's MachineID
    coordinate = Coordinate(p.xid, p.yid);

    rowSize = p.row_size;
    colSize = p.col_size;
}

void
SharingManager::init()
{
    DPRINTF(SharingManager, "Current coordinate is %s\n", coordinate.toString());
    for (AbstractController* cntrl : downstreamHNFs) {
        MachineID id = cntrl->getMachineID();
        DPRINTF(SharingManager, "Iterating through HNF %s\n", MachineIDToString(id));
        for (const AddrRange &range : cntrl->getAddrRanges()) {
            if (hnfMap.intersects(range) != hnfMap.end()) {
                fatal("HNF %s range %s overlaps others\n", cntrl->name(), range.to_string());
            }
            DPRINTF(SharingManager, "Inserted addr range %s to hnfMap\n", range.to_string());
            hnfMap.insert(range, id);
        }
        Coordinate coord = Coordinate(cntrl->getXid(), cntrl->getYid());
        DPRINTF(SharingManager, "Inserted hnf name %s id %s coordinate %s\n",
            cntrl->name(), MachineIDToString(id), coord.toString());
        slaveCoordinateMap.insert({id, coord});
    }
    for (AbstractController* cntrl : downstreamSNFs) {
        MachineID id = cntrl->getMachineID();
        DPRINTF(SharingManager, "Iterating through SNF %s\n", MachineIDToString(id));
        for (const AddrRange &range : cntrl->getAddrRanges()) {
            if (snfMap.intersects(range) != snfMap.end()) {
                fatal("SNF %s range %s overlaps others\n", cntrl->name(), range.to_string());
            }
            DPRINTF(SharingManager, "Inserted addr range %s to snfMap\n", range.to_string());
            snfMap.insert(range, cntrl->getMachineID());
        }
        // TODO use gecoordinate instead
        Coordinate coord = Coordinate(cntrl->getXid(), cntrl->getYid());
        DPRINTF(SharingManager, "Inserted snf name %s id %s coordinate %s\n",
            cntrl->name(), MachineIDToString(id), coord.toString());
        slaveCoordinateMap.insert({id, coord});
    }
    for (AbstractController* cntrl : rnfs) {
        Coordinate c = Coordinate(cntrl->getXid(), cntrl->getYid());
        MachineID id = cntrl->getMachineID();
        RNFCoordinateMap.insert({c, id});
        DPRINTF(SharingManager, "Added rnf name %s id %s coordinate %s\n",
            cntrl->name(), MachineIDToString(id), c.toString());
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
    for (auto it = sharingTable.begin(); it != sharingTable.end(); it++) {
        if (it->addr == addr) {
            sharingTable.erase(it);
            break;
        }
    }
}

/* check if a given address is shared */
SharingDirection
SharingManager::getSharingDirection(Addr addr) const
{
    for (auto it = sharingTable.begin(); it != sharingTable.end(); it++) {
        if (it->addr == addr) {
            return it->direction;
        }
    }
    return SharingDirection::NONE;
}

NeighbourCoords
Coordinate::naiveNeighbour(Coordinate slave, SharingDirection direction) const
{
    int down_xid = xid;
    int down_yid = yid;
    int up_xid = xid;
    int up_yid = yid;
    Coordinate downstream = Coordinate(xid, yid);
    Coordinate upstream_0 = Coordinate(xid, yid);
    Coordinate upstream_1 = Coordinate(xid, yid);

    if (direction == SharingDirection::ROW) {
        // same yid, minimize xid diff
        if (slave.xid < xid) {
            downstream.xid = xid - 1;
            upstream_0.xid = xid + 1;
            upstream_1 = Coordinate();
        } else if (slave.xid > xid) {
            downstream.xid = xid + 1;
            upstream_0 = Coordinate();
            upstream_1.xid = xid - 1;
        } else {
            upstream_0.xid = xid + 1;
            upstream_1.xid = xid - 1;
        }
    } else if (direction == SharingDirection::COL) {
        // same xid, minimize yid diff
        if (slave.yid < yid) {
            downstream.yid = yid - 1;
            upstream_0.yid = yid + 1;
            upstream_1 = Coordinate();
        } else if (slave.yid > yid) {
            downstream.yid = yid + 1;
            upstream_0 = Coordinate();
            upstream_1.yid = yid - 1;
        } else {
            upstream_0.yid = yid + 1;
            upstream_1.yid = yid - 1;
        }
    }
    return NeighbourCoords{downstream, upstream_0, upstream_1};
}

// addr -> slave MachineID
MachineID
SharingManager::getSlaveID(Addr addr) const {
    auto slave_mapping = snfMap;
    std::map<AddrRange, MachineID>::iterator it = slave_mapping.contains(addr);
    if (it != slave_mapping.end()) {
        DPRINTF(SharingManager, "GetSlaveID: Addr %lx current slave id %d\n",
            addr, MachineIDToString(it->second));
        return it->second;
    } else {
        fatal("SharingManager: No downstream destination for %s\n", addr);
    }

}

Coordinate
SharingManager::getSlaveCoordinate(MachineID id) const {
    Coordinate c = slaveCoordinateMap.at(id);
    DPRINTF(SharingManager, "get slave coordinate @ %s\n", c.toString());
    return slaveCoordinateMap.at(id);
}

MachineID
SharingManager::getUpstream_0(Addr addr) {
    Neighbour neighbour = checkInsertNeighbour(addr);
    return neighbour.upstream_0;
}

MachineID
SharingManager::getUpstream_1(Addr addr) {
    Neighbour neighbour = checkInsertNeighbour(addr);
    return neighbour.upstream_1;
}

MachineID
SharingManager::getDownstream(Addr addr) {
    Neighbour neighbour = checkInsertNeighbour(addr);
    return neighbour.downstream;
}

Neighbour
SharingManager::checkInsertNeighbour(Addr addr) {
      // 1. get direction
      // 2. get slaveID
      auto direction = getSharingDirection(addr);
      auto slaveID = getSlaveID(addr);
      auto chainProp = ChainProperty(direction, slaveID);
      auto n = neighbourTable.find(chainProp);
      if (n != neighbourTable.end()) {
        Neighbour neighbour = n->second;
        DPRINTF(SharingManager, "Addr %s Found cached neighbour %s\n", addr, neighbour.toString());
        return neighbour;
      }
      DPRINTF(SharingManager, "Inserting new neighbour for coordinate %s addr %lx\n",
          coordinate.toString(), addr);
      // Append new entry to neighbour Table
      Coordinate slaveCoordinate;
      Coordinate upCoord, downCoord;
      MachineID upstream, downstream;
      auto slave_mapping = snfMap;
      Neighbour new_neighbour;

      slaveCoordinate = getSlaveCoordinate(slaveID);

      NeighbourCoords naive_neighbour_coords =
        coordinate.naiveNeighbour(slaveCoordinate, direction);

      Coordinate down_coord = naive_neighbour_coords.downstream;
      Coordinate up_coord_0 = naive_neighbour_coords.upstream_0;
      Coordinate up_coord_1 = naive_neighbour_coords.upstream_1;

      // Check coordinates exists
      // 1. Check if downstream is itself
      if (down_coord == coordinate) {
        //
        DPRINTF(SharingManager, "adding downstream SNF coordinate %s\n",
          slaveCoordinate.toString());
        new_neighbour.downstream = slaveID;
      } else if (validCoordinate(down_coord)) {
        DPRINTF(SharingManager, "adding downstream RNF coordinate %s\n",
          down_coord.toString());
        new_neighbour.downstream = RNFCoordinateMap.at(down_coord);
      } else {
        fatal("No valid downstream for addr %s current coord %s and naive down coord %s\n",
          addr, coordinate.toString(), down_coord.toString());
      }

      if (up_coord_0 != coordinate && validCoordinate(up_coord_0)) {

        new_neighbour.upstream_0 = RNFCoordinateMap.at(up_coord_0);
        DPRINTF(SharingManager, "adding upstream0 coordinate %s\n",
          up_coord_0.toString());
      } else {
        DPRINTF(SharingManager, "No valid upstream0 for naive coord %s", up_coord_0.toString());
        // new_neighbour.upstream_0 is MachineID()
      }

      if (up_coord_1 != coordinate && validCoordinate(up_coord_1)) {
        new_neighbour.upstream_1 = RNFCoordinateMap.at(up_coord_1);
        DPRINTF(SharingManager, "adding upstream 1 coordinate %s \n",
          up_coord_1.toString());
      } else {
        DPRINTF(SharingManager, "No valid upstream1 for naive coord %s", up_coord_1.toString());
      }

      // Check if downstream exists
      assert (new_neighbour.downstream.isValid());

      neighbourTable.insert({chainProp, new_neighbour});
      DPRINTF(SharingManager, "Added new neighbour addr %lx chainProp %s, neighbour%s\n",
          addr, chainProp.toString(), new_neighbour.toString());
      return new_neighbour;
    }

// MachineID
// SharingManager::getDestination(Addr addr, SharingDirection direction) const
// {
//     auto slave_mapping = snfMap;
//     std::map<AddrRange, MachineID>::iterator c = slave_mapping.contains(addr);
//     Coordinate slave_coord;
//     if (c != slave_mapping.end()) {
//         slave_coord = slaveCoordinateMap.at(c->second);
//         DPRINTF(SharingManager, "Addr %lx current id  coord %s, slave id %d coord %s\n",
//             addr, id, coordinate, c->second, slave_coord);
//     } else {
//         fatal("SharingManager: No downstream destination for %s\n", addr);
//     }

//     // Neighbouring grid coordinate, may not have RN there
//     NeighbourCoords naive_neighbour = coordinate.naiveNeighbour(slave_coord, direction);

//     if (naive_neighbour.downstream == slave_coord) {
//         // RN and slave on same grid, dest is slave
//         DPRINTF(SharingManager, "Naive neighour is slave\n");
//         return c->second;
//     }

//     auto rnf_it = RNFCoordinateMap.find(naive_neighbour.upstream_0);
//     if (rnf_it == RNFCoordinateMap.end()) {
//         // Naive neighbour does not map to RN,
//         // means current RN is closest to slave
//         DPRINTF(SharingManager, "Current RN is closest to slave\n");
//         return c->second;
//     }

//     DPRINTF(SharingManager, "Next target is neighbouring RN id %d coor %s\n",
//         rnf_it->second, naive_neighbour.to_string());
//     return rnf_it->second;
// }

// Get previous holder of target
// MachineID
// SharingManager::getBackwardTgt(Addr addr, SharingDirection direction) const
// {

// }

}
}
