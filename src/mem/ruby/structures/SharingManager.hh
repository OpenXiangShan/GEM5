#ifndef __MEM_RUBY_STRUCTURES_SHARING_MANAGER_HH__
#define __MEM_RUBY_STRUCTURES_SHARING_MANAGER_HH__

#include <vector>

#include "base/addr_range.hh"
#include "base/addr_range_map.hh"
#include "mem/cache/base.hh"
#include "mem/ruby/common/HasDownStream.hh"
#include "mem/ruby/common/MachineID.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"
#include "params/SharingManager.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace ruby
{

class AbstractController;

enum SharingDirection { NONE = 0, ROW, COL };

class SharingManager : public SimObject
{
  public:
    typedef SharingManagerParams Params;

    SharingManager(const Params &p);
    ~SharingManager();
    void init();

    void setSharing(Addr addr, Addr size, SharingDirection direction);
    void clearSharing(Addr addr);
    SharingDirection getSharingDirection(Addr addr);

    MachineID getDestination(Addr addr, SharingDirection direction) const;
    MachineID getUpstream(Addr addr, SharingDirection direction) const;

    struct SharingEntry
    {
        Addr addr;
        Addr size;
        SharingDirection direction;
        SharingEntry(Addr addr, Addr size, SharingDirection direction) :
            addr(addr), size(size), direction(direction)
        {}
    };

    struct Coordinate
    {
        int xid;
        int yid;
        Coordinate() : xid(-1), yid(-1) {}
        Coordinate(int xid, int yid) : xid(xid), yid(yid) {}
        Coordinate east() { return Coordinate(xid - 1, yid); }
        Coordinate west() { return Coordinate(xid + 1, yid); }
        Coordinate north() { return Coordinate(xid, yid - 1); }
        Coordinate south() { return Coordinate(xid, yid + 1); }

        std::pair<Coordinate, std::pair<Coordinate, Coordinate>>
        NaiveNeighbour(Coordinate slave, SharingDirection direction) const;

        bool operator==(const Coordinate& other) const {
          return xid == other.xid && yid == other.yid;
        }

        std::string toString() const {
          return "(" + std::to_string(xid) + ", " + std::to_string(yid) + ")";
        }
    };

    typedef std::pair<Coordinate, Coordinate> UpCoordPair;

    template<>
    std::hash<Coordinate> {
      std::size_t operator()(const Coordinate& c) const {
        return std::hash<int>()(c.xid + c.yid << 64);
      }
    }

    std::hash<MachineID> {
      std::size_t operator()(const MachineID& m) const {
        return std::hash<int>()(m.id);
      }
    };

    struct ChainProperty
    {
      SharingDirection direction;
      MachineID slave;
      ChainProperty(SharingDirection d, MachineID s) : direction(d), slave(s) {}
    };

    std::hash<ChainProperty> {
      std::size_t operator()(const ChainProperty& c) const {
        return std::hash<int>()(c.direction + c.slave.id << 2);
      }
    };

    struct Neighbour
    {
      // TODO: there should be 2 upstreams
      // .first is upstream with larger mesh index
      std::pair<MachineID, MachineID> upstream;
      MachineID downstream;
    };


  private:
    MachineID id;
    std::vector<SharingEntry> sharingTable;
    // Addr -> SlaveID
    // slaveID -> SlaveCoordinate
    // Direction + SlaveID/SlaveCoordinate + CurrentCoord-> upstreamCoordinate & downstreamCoordinate
    // (up|down)streamCoordinate -> (up|down)stream MachineID
    std::unordered_map<ChainProperty, Neighbour> neighbourTable;
public:
    MachineID getSlaveID(Addr addr) const;

    void checkNeighbour(Addr addr) {
      // 1. get direction
      // 2. get slaveID
      auto direction = getSharingDirection(addr);
      auto slaveID = getSlaveID(addr);
      auto chainProp = ChainProperty(direction, slaveID);
      auto n = neighbourTable.find(chainProp);
      if (n != neighbourTable.end()) {
        Neighbour neighbour = n->second;
        // TODO
        return;
      }
      // Append new entry to neighbour Table
      Coordinate slaveCoordinate;
      Coordinate upCoord, downCoord;
      MachineID upstream, downstream;
      auto slave_mapping = snfMap;
      ChainProperty chainProp(direction, slaveID);
      Neighbour new_neighbour;

      std::pair<Coordinate, std::pair<Coordinate, Coordinate>> neighbour_coord =
        coordinate.NaiveNeighbour(slaveCoordinate, direction);

      Coordinate down_coord = neighbour_coord.first;
      Coordinate up_coord_0 = neighbour_coord.second.first;
      Coordinate up_coord_1 = neighbour_coord.second.second;

      // Check coordinates exists
      // 1. Check if downstream is itself
      if (down_coord == coordinate) {
        //
        DPRINTF("RNF %s Adding downstream for Slave coordinate %s\n",
          coordinate,
          slaveCoordinate);
        new_neighbour.downstream = slave_coord;
      }

      neighbourTable.insert(std::make_pair(chainProp, new_neighbour));
      return;
    }

private:
    AbstractController *controller;
    std::vector<AbstractController *> downstreamHNFs;
    std::vector<AbstractController *> downstreamSNFs;
    std::vector<AbstractController *> rnfs;
    AddrRangeMap<MachineID, 3> hnfMap;
    AddrRangeMap<MachineID, 3> snfMap;
    Coordinate coordinate;
    std::unordered_map<Coordinate, MachineID> RNFCoordinateMap;
    std::unordered_map<MachineID, Coordinate> slaveCoordinateMap;

  public:
    Coordinate getCoordinate() const { return coordinate; }
};
}
}

#endif
