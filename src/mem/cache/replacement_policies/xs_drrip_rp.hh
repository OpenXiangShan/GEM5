/**
 * Copyright (c) 2024 XiangShan
 * All rights reserved.
 *
 * Unified implementation of XiangShan RRIP replacement policy series
 *
 * This class unifies three replacement policies: SRRIP, BRRIP, and DRRIP:
 * - SRRIP mode: Static Re-reference Interval Prediction
 * - BRRIP mode: Bimodal Re-reference Interval Prediction (more pessimistic)
 * - DRRIP mode: Dynamic selection between SRRIP and BRRIP using Set Dueling
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_XS_DRRIP_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_XS_DRRIP_RP_HH__

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "mem/cache/replacement_policies/base.hh"

namespace gem5
{

struct XSDRRIPRPParams;

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

/**
 * Unified XiangShan RRIP replacement policy
 *
 * Supports three working modes:
 * 1. SRRIP: All sets use SRRIP logic (insert with RRPV=2)
 * 2. BRRIP: All sets use BRRIP logic (insert with RRPV=3, more pessimistic)
 * 3. DRRIP: Dynamically select between SRRIP and BRRIP using Set Dueling
 *
 * The only difference between SRRIP and BRRIP:
 * - For "non-prefetch release first-use" and "prefetch release"
 * - SRRIP inserts with RRPV=2, BRRIP inserts with RRPV=3
 */
class XSDRRIP : public Base
{
  public:
    // Working mode
    enum Mode
    {
        SRRIP_MODE,   // Static RRIP
        BRRIP_MODE,   // Bimodal RRIP
        DRRIP_MODE    // Dynamic RRIP (Set Dueling)
    };

  protected:
    // Unified replacement data structure (shared by all modes)
    struct XSDRRIPReplData : ReplacementData
    {
        // Re-Reference Prediction Value (2-bit saturating counter)
        // 0 = near-immediate re-reference, 3 = distant re-reference
        SatCounter8 rrpv;

        // Whether the entry is valid
        bool valid;

        // Origin bit: Marks whether this block is first-use or reuse
        // - false (0): First-use (block has never been accessed)
        // - true (1): Reuse (block has been accessed at least once)
        // Used for req_type bit[3]
        bool originBit;

        // Set ID (used only in DRRIP mode)
        // Used in Set Dueling to determine which policy to use
        unsigned setId;

        // Set type (used only in DRRIP mode)
        // Pre-computed in instantiateEntry() to avoid repeated calculation
        // 0 = SRRIP dedicated set
        // 1 = BRRIP dedicated set
        // 2 = Follower set
        uint8_t setType;

        // Default constructor
        XSDRRIPReplData() : rrpv(2), valid(false), originBit(false), setId(0), setType(2)
        {
        }
    };

    // Maximum RRPV value
    static constexpr unsigned MAX_RRPV = 3;

    // Set index bit manipulation for Set Dueling
    // These are computed from numSets in constructor
    unsigned setBits;      // log2(numSets)
    unsigned halfSetBits;  // setBits / 2
    uint32_t setMask;      // Mask for low half bits

    // PSEL counter bit width (used only in DRRIP mode)
    //  TODO: Not frequently modified, temporarily use constants.
    static constexpr unsigned PSEL_WIDTH = 10;

    // PSEL counter maximum value
    static constexpr unsigned PSEL_MAX = (1 << PSEL_WIDTH);

    // PSEL threshold (>= 512 uses BRRIP, < 512 uses SRRIP)
    static constexpr unsigned PSEL_THRESHOLD = PSEL_MAX / 2;

    // Current working mode
    const Mode mode;

    // Total number of sets in cache (used only in DRRIP mode)
    const unsigned numSets;

    // Number of ways per set
    const unsigned numWays;

    // Policy Selection Counter (used only in DRRIP mode)
    // Tracks relative performance of SRRIP and BRRIP
    mutable SatCounter32 pselCounter;

    // Current number of instantiated entries
    // Used for allocating set IDs
    unsigned entryCount;

    // Set Dueling related statistics (used only in DRRIP mode)
    mutable struct XSDRRIPStats : public statistics::Group
    {
        XSDRRIPStats(statistics::Group* parent);

        /** Number of misses in SRRIP region */
        statistics::Scalar srripMisses;
        /** Number of misses in BRRIP region */
        statistics::Scalar brripMisses;
        /** Number of times follower region used SRRIP */
        statistics::Scalar followerUseSrrip;
        /** Number of times follower region used BRRIP */
        statistics::Scalar followerUseBrrip;
        /** Current PSEL counter value */
        statistics::Scalar pselValue;
    } duelingStats;

    /**
     * Decide whether to use BRRIP or SRRIP logic
     *
     * For DRRIP mode, uses pre-computed setType from replacement_data:
     * - setType 0 (SRRIP dedicated): return false
     * - setType 1 (BRRIP dedicated): return true
     * - setType 2 (Follower): query PSEL MSB
     *
     * @param replacement_data Replacement data containing pre-computed setType
     * @return true=use BRRIP logic, false=use SRRIP logic
     */
    bool useBRRIP(const std::shared_ptr<ReplacementData>& replacement_data) const;

    /**
     * Determine RRPV value for a cache access
     *
     * Extracts access characteristics from packet and decides RRPV based on:
     * - Access type (first-use vs reuse)
     * - Operation type (acquire vs release)
     * - Request source (demand vs prefetch)
     * - Cache status (hit vs refill)
     * - Replacement mode (SRRIP vs BRRIP)
     *
     * Internal encoding matches XiangShan RTL req_type:
     *   bit[3]: 0=first-use, 1=reuse
     *   bit[2]: 0=acquire(read), 1=release(write/evict)
     *   bit[1]: 0=non-prefetch, 1=prefetch
     *   bit[0]: 0=non-refill, 1=refill
     *
     * @param pkt Packet pointer
     * @param replacement_data Replacement data (used to read originBit)
     * @param use_brrip Whether to use BRRIP logic (vs SRRIP)
     * @return RRPV value (0-3)
     */
    uint8_t getRRPV(const PacketPtr pkt,
                    const std::shared_ptr<ReplacementData>& replacement_data,
                    bool use_brrip) const;

  public:
    typedef XSDRRIPRPParams Params;
    XSDRRIP(const Params &p);
    ~XSDRRIP() = default;

    /**
     * Invalidate replacement data
     */
    void invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
                                                                    override;

    /**
     * Touch entry (called on hit)
     */
    void touch(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt) override;

    void touch(const std::shared_ptr<ReplacementData>& replacement_data) const
                                                                     override;

    /**
     * Reset entry (called on miss refill)
     * Updates PSEL counter in DRRIP mode
     */
    void reset(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt) override;

    void reset(const std::shared_ptr<ReplacementData>& replacement_data) const
                                                                     override;

    void reset4memtrace(const std::shared_ptr<ReplacementData>& replacement_data,int priority) const override
    {
    }
    /**
     * Find victim for replacement
     */
    ReplaceableEntry* getVictim(const ReplacementCandidates& candidates) const
                                                                     override;

    /**
     * Instantiate replacement data
     */
    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_XS_DRRIP_RP_HH__
