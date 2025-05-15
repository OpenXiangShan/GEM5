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

        Coordinate NaiveNeighbour(Coordinate slave, SharingDirection direction) const;

        bool operator==(const Coordinate& other) const {
          return xid == other.xid && yid == other.yid;
        }

        std::string toString() const {
          return "(" + std::to_string(xid) + ", " + std::to_string(yid) + ")";
        }
    };

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
    }


  private:
    MachineID id;
    std::vector<SharingEntry> sharingTable;
    AbstractController *controller;
    std::vector<AbstractController *> downstreamHNFs;
    std::vector<AbstractController *> downstreamSNFs;
    std::vector<AbstractController *> rnfs;
    AddrRangeMap<MachineID, 3> hnfMap;
    AddrRangeMap<MachineID, 3> snfMap;
    // Records coordinate to ID mapping of RNFs
    Coordinate coordinate;
    std::unordered_map<Coordinate, MachineID> RNFCoordinateMap;
    std::unordered_map<MachineID, Coordinate> slaveCoordinateMap;

  public:
    Coordinate getCoordinate() const { return coordinate; }
};
}
}

#endif
