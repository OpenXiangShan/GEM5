#pragma once
#include <bitset>
#include <cassert>
#include <cstdint>
#include <list>

#include "base/intmath.hh"

namespace gem5
{
namespace xsCHI
{
    // class SystemAddressMap
    // {
    // public:
    //     SystemAddressMap() = default;
    //     // ~SystemAddressMap() = default;
    //     // 获取目标ID
    //     virtual uint32_t getTargetID(uint64_t addr) = 0;
    // };

    class SystemAddressMapRN 
    {
        // Transactions from an RN must pass through an RN SAM to generate a CHI target ID. The target ID
        // is used to send the flit to the correct target node in the mesh.
        // CMN‑700 RN-Ds, RN‑Fs, RN-Is, and CCG HAs use an RN SAM that is internal to the interconnect.
        // When multiple RN-Fs are connected using CAL, only on RN SAM exists per CAL.
        // The RN SAM uses two characteristics of a transaction to map requests to downstream target
        // nodes:
        // • The Physical Address (PA) of the request
        // • Whether the request is a DVM operation, a PrefetchTgt operation, or neither
        // The RN SAM also has a defined default target, the HN-D. It uses the default target if the preceding
        // characteristics do not result in a match or the RN SAM has not been programmed yet.

        // SCG: Power of two hashing, legacy CMN mode
        //  Power of two hashing supports hashing over 1, 2, 4, 8, 16, 32, 64, 128, or 256 HN‑Fs. The hash
        //  function uses bits [MSB:6] of the PA of the request. To use power of two hashing over 256 HN‑F
        //  nodes, you must enable CAL mode. For more information, see 3.4.6.6 HN‑F with CAL support on
        //  page 132.
        //  If CMN‑700 has been configured to implement fewer than 52 PA bits, the unused upper bits are
        //  assumed to be zero. The hash algorithm calculates a pointer in the HN‑F ID table in the RN SAM.
        //  The hash function is explicitly given in the following list. All numbers on the right-hand side of the
        //  equations in the list are bit positions within the PA. For example, 17 corresponds to PA bit[17]. In
        //  the equations, ^ represents XOR.
        //  • Two HN‑Fs:
        //  ◦ Number of bits in select: 1
        //  ◦ select [0] = (6^7^8^…^51)
        //  • Four HN‑Fs:
        //  ◦ Number of bits in select: 2
        //  ◦ select [0] = (6^8^10^…^50)
        //  ◦ select [1] = (7^9^11^…^51)
        private:
            std::list<uint32_t> HNF_NodeIDs; // 存储目标ID列表
            uint32_t SelectBits; // 选择位数，表示HNF_NodeIDs的大小
        public:
            // SystemAddressMapRN() = default;
            SystemAddressMapRN() : HNF_NodeIDs(0), SelectBits(0) {
            }
            void addNodeID(uint32_t nodeID) {
                HNF_NodeIDs.push_back(nodeID);
                // 验证节点数量是2的幂
                size_t size = HNF_NodeIDs.size();
                // assert((size & (size - 1)) == 0 && "NodeList size must be power of 2");
                SelectBits = floorLog2(size);
            }
            // ~SystemAddressMapRN() = default;
            // 获取目标ID
            uint32_t getTargetID(uint64_t addr)  {
                // HNF_NodeIDs.size() 必为2的幂
                if (HNF_NodeIDs.size()==1){
                    return *(HNF_NodeIDs.begin());
                }
                // 计算每一位select[i]，每位的起始bit和步长按规则推导
                uint32_t index = 0;
                for (uint32_t i = 0; i < SelectBits; ++i) {
                    // 起始bit = 6 + i
                    // 步长 = SelectBits
                    uint32_t xor_bit = 0;
                    bool first = true;
                    for (uint32_t b = 6 + i; b <= 51; b += SelectBits) {
                        uint32_t bit_val = (addr >> b) & 0x1;
                        if (first) {
                            xor_bit = bit_val;
                            first = false;
                        } else {
                            xor_bit ^= bit_val;
                        }
                    }
                    index |= (xor_bit << i);
                }
                // 获取index对应的NodeID
                auto it = HNF_NodeIDs.begin();
                std::advance(it, index);
                return *it;
            };
    };
    class SystemAddressMapHN 
    {
        private:
            std::list<uint32_t> HNF_NodeIDs; // 存储目标ID列表
            uint32_t SelectBits; // 选择位数，表示HNF_NodeIDs的大小
        public:
            SystemAddressMapHN() : HNF_NodeIDs(0), SelectBits(0) {
            }
            void addNodeID(uint32_t nodeID) {
                HNF_NodeIDs.push_back(nodeID);
                // 验证节点数量是2的幂
                size_t size = HNF_NodeIDs.size();
                // assert((size & (size - 1)) == 0 && "NodeList size must be power of 2");
                SelectBits = floorLog2(size);
            }
            // ~SystemAddressMapRN() = default;
            // 获取目标ID
            uint32_t getTargetID(uint64_t addr)  {
                // HNF_NodeIDs.size() 必为2的幂
                if (HNF_NodeIDs.size()==1){
                    return *(HNF_NodeIDs.begin());
                }
                // 计算每一位select[i]，每位的起始bit和步长按规则推导
                uint32_t index = 0;
                for (uint32_t i = 0; i < SelectBits; ++i) {
                    // 起始bit = 6 + i
                    // 步长 = SelectBits
                    uint32_t xor_bit = 0;
                    bool first = true;
                    for (uint32_t b = 6 + i; b <= 51; b += SelectBits) {
                        uint32_t bit_val = (addr >> b) & 0x1;
                        if (first) {
                            xor_bit = bit_val;
                            first = false;
                        } else {
                            xor_bit ^= bit_val;
                        }
                    }
                    index |= (xor_bit << i);
                }
                // 获取index对应的NodeID
                auto it = HNF_NodeIDs.begin();
                std::advance(it, index);
                return *it;
            };
    };
}}
