#ifndef __MEM_RUBY_STRUCTURES_SHARING_MANAGER_HH__
#define __MEM_RUBY_STRUCTURES_SHARING_MANAGER_HH__

#include <array>
#include <unordered_map>
#include <vector>

#include "base/addr_range.hh"
#include "base/addr_range_map.hh"
#include "base/trace.hh"
#include "debug/SharingManager.hh"
#include "mem/cache/base.hh"
#include "mem/ruby/common/HasDownStream.hh"
#include "mem/ruby/common/MachineID.hh"
#include "mem/ruby/protocol/MachineType.hh"
#include "params/SharingManager.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace ruby
{

enum SharingDirection { NONE = 0, ROW, COL };

struct MachineIDHash
{
  std::size_t operator()(const MachineID& m) const {
    return std::hash<int>()(m.getNum());
  }
};

struct NeighbourCoords;

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

  NeighbourCoords naiveNeighbour(Coordinate slave, SharingDirection direction) const;

  bool operator==(const Coordinate& other) const {
    return xid == other.xid && yid == other.yid;
  }

  bool operator!=(const Coordinate& other) const {
    return xid != other.xid || yid != other.yid;
  }

  std::string toString() const {
    return "(" + std::to_string(xid) + ", " + std::to_string(yid) + ")";
  }
};

struct CoordinateHash
{
  std::size_t operator()(const Coordinate& c) const {
    return std::hash<int>()(c.xid + (c.yid << 16));
  }
};

struct ChainProperty
{
  SharingDirection direction;
  MachineID slave;
  ChainProperty(SharingDirection d, MachineID s) : direction(d), slave(s) {}

  bool operator==(const ChainProperty &other) const {
    return direction == other.direction && slave == other.slave;
  }

  std::string toString () const {
    return "[D:" + std::to_string(direction) + ", S:" + MachineIDToString(slave) + "]";
  }
};

struct ChainPropertyHash
{
  std::size_t operator()(const ChainProperty& c) const {
    return std::hash<int>()(static_cast<int>(c.direction) + (c.slave.getNum() << 2));
  }
};

// Internal representation for gei
struct NeighbourCoords
{
  Coordinate downstream;
  Coordinate upstream_0, upstream_1;
  NeighbourCoords(Coordinate d, Coordinate u0, Coordinate u1) :
    downstream(d), upstream_0(u0), upstream_1(u1) {}
};

struct Neighbour
{
  // .first is upstream with larger mesh index
  MachineID downstream;
  MachineID upstream_0, upstream_1;
  Neighbour() : downstream(MachineID()),
    upstream_0(MachineID()), upstream_1(MachineID()) {}

  bool operator==(const Neighbour &other) const {
    return downstream == other.downstream && upstream_0 == other.upstream_0 &&
      upstream_1 == other.upstream_1;
  }

  std::string toString() const {
    return "Down " + MachineIDToString(downstream) + ", Up0 " + MachineIDToString(upstream_0) +
      ", Up1 " + MachineIDToString(upstream_1);
  }

};

struct NeighbourHash
{
  std::size_t operator()(const Neighbour& n) const {
    return (MachineIDHash()(n.downstream) << 32 | MachineIDHash()(n.upstream_0) << 16 | MachineIDHash()(n.upstream_1));
  }
};

// template<>
// struct std::hash<gem5::ruby::MachineID> {
//   std::size_t operator()(const gem5::ruby::MachineID& m) const {
//     return std::hash<int>()(m.getNum());
//   }
// };

class SharingManager : public SimObject
{
  public:
    typedef SharingManagerParams Params;

    SharingManager(const Params &p);
    ~SharingManager();
    void init();

    // Set sharing
    void setSharing(Addr addr, Addr size, SharingDirection direction);
    void clearSharing(Addr addr);
    SharingDirection getSharingDirection(Addr addr) const;

    // MachineID getDestination(Addr addr, SharingDirection direction) const;
    MachineID getUpstream_0(Addr addr);
    MachineID getUpstream_1(Addr addr);
    MachineID getDownstream(Addr addr);

    struct SharingEntry
    {
        Addr addr;
        Addr size;
        SharingDirection direction;
        SharingEntry(Addr addr, Addr size, SharingDirection direction) :
            addr(addr), size(size), direction(direction)
        {}
    };

    // Forward declare hash specializations
    // struct std::hash<Coordinate>;
    // struct std::hash<MachineID>;
    // struct std::hash<ChainProperty>;

  private:
  // current machineID
    MachineID id;
    // addr -> sharing direction, TODO use addr range map
    std::vector<SharingEntry> sharingTable;

    std::unordered_map<ChainProperty, Neighbour, ChainPropertyHash> neighbourTable;
public:
    MachineID getSlaveID(Addr addr) const;

    Coordinate getSlaveCoordinate(MachineID slaveID) const;

    bool validCoordinate(Coordinate coord) const {
      return (coord.xid >= 0 && coord.xid < colSize) &&
        (coord.yid >= 0 && coord.yid < rowSize);
    }

    Neighbour checkInsertNeighbour(Addr addr);

    // Addr -> SlaveID
    // slaveID -> SlaveCoordinate
    // Direction + SlaveCoordinate + CurrentCoord-> upstreamCoordinate & downstreamCoordinate
    // (up|down)streamCoordinate -> (up|down)stream MachineID

    // Same slave, direction => Same upstream/downstream


private:
    AbstractController *controller;
    std::vector<AbstractController *> downstreamHNFs;
    std::vector<AbstractController *> downstreamSNFs;
    std::vector<AbstractController *> rnfs;
    // Map Addr ranges to MachineID
    AddrRangeMap<MachineID, 3> hnfMap;
    AddrRangeMap<MachineID, 3> snfMap;
    // Current coordinate
    Coordinate coordinate;
    // RNF Coordinate to slaveID mapping
    std::unordered_map<Coordinate, MachineID, CoordinateHash> RNFCoordinateMap;
    // Slave MachineID to coordinate Mapping
    std::unordered_map<MachineID, Coordinate, MachineIDHash> slaveCoordinateMap;

    // Mesh
    int rowSize, colSize;

  public:
    Coordinate getCoordinate() const { return coordinate; }
  };

}
}

#endif
