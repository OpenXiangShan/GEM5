#pragma once
#include <cassert>
#include <cmath>
#include <cstdint>

namespace gem5
{

namespace xsCHI
{
    // NodeID : The physical position of a device in the mesh determines the node ID mapping
    class NodeID
    {
        private:
        // 1. The X coordinate of its XP
        // 2. The Y coordinate of its XP
        // 3. The XP device port (0 or 1) that it connects to
        // The device node ID is mapped to (X, Y, Port, 0b00).
        const int mesh_coord_bits = 5;//maxium 32*32 mesh supported
        uint32_t  X_coord;
        uint32_t  Y_coord;
        uint32_t  port; // 0 or 1

    public:
        NodeID(uint32_t x, uint32_t y, uint32_t portID)
            : X_coord(x), Y_coord(y), port(portID)
        {
            assert(port < 2);
            assert(x < 1u<<mesh_coord_bits);
            assert(y < 1u<<mesh_coord_bits);
        }
        NodeID(const NodeID& other) = default;
        // ~NodeID() = default;

        uint32_t getNodeID() const
        {
            assert(port<2);
            uint32_t Xbit = X_coord&((1<<mesh_coord_bits)-1);
            uint32_t Ybit = Y_coord&((1<<mesh_coord_bits)-1);
            if (port == 0) {
                return ((Xbit<<mesh_coord_bits)|Ybit)<<3;
            }else{
                return (((Xbit<<mesh_coord_bits)|Ybit)<<3) | 0b100;
            }
        }

        uint32_t getXCoord() const { return X_coord; }
        uint32_t getYCoord() const { return Y_coord; }
        uint32_t getPort() const { return port; }

        NodeID createFromNodeID(uint32_t node_id) const {
            // Extract X, Y, and port from the node ID
            uint32_t port = (node_id & 0b100) >> 2 ;
            uint32_t coords = node_id >> 3; // Remove the low 3 bits
            uint32_t X_coord = (coords >> mesh_coord_bits) & ((1 << mesh_coord_bits) - 1);
            uint32_t Y_coord = coords & ((1 << mesh_coord_bits) - 1);
            return NodeID(X_coord, Y_coord, port);
        }
         bool operator=(const NodeID& other) const{
            return X_coord == other.X_coord &&
                   Y_coord == other.Y_coord &&
                   port == other.port;
        }
    };
}}
