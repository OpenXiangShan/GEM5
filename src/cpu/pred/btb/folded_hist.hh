#ifndef __CPU_PRED_BTB_FOLDED_HIST_HH__
#define __CPU_PRED_BTB_FOLDED_HIST_HH__

#include <array>
#include <cstdint>

#include <boost/dynamic_bitset.hpp>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/btb/stream_struct.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

// PHR hash related
constexpr static uint64_t pathHashLength = 15;

inline uint64_t
pathHash(const Addr branchPC, const Addr targetPC)
{
    uint64_t hash = ((((branchPC >> 1) & ((1ULL << 9) - 1)) << 4) ^ ((targetPC >> 2) & ((1ULL << 15) - 1)));
    hash &= ((1ULL << pathHashLength) - 1);
    return hash;
}

inline uint64_t
foldHash(uint64_t hash, const std::size_t foldedLen)
{
    int bitsLeft = pathHashLength;
    uint64_t foldedHash = 0;
    while (bitsLeft > 0) {
        foldedHash ^= hash;
        hash >>= foldedLen;
        bitsLeft -= foldedLen;
    }
    return foldedHash;
}

/**
 * FoldedHistBase implements a folded history mechanism for branch prediction.
 * It maintains a compressed version of branch history by XORing multiple history bits
 * into a smaller number of bits, which helps reduce storage while preserving
 * correlation information.
 *
 * This is an abstract base class. Concrete implementations include:
 * - DirectionFoldedHist: For branch direction histories (global, global backward, local)
 * - ImliFoldedHist: For IMLI (Irregular Multiple-Loop Iteration) history
 * - PathFoldedHist: For path history tracking branch targets
 */
class FoldedHistBase
{
  protected:
    constexpr static int staticMaxShamtLimit = 16;
    std::size_t histLen;    // Length of the original history
    std::size_t foldedLen;  // Length of the folded (compressed) history
    std::size_t maxShamt;   // Maximum shift amount for history updates
    uint64_t _folded;       // The folded history bits

    // Pre-calculated positions for efficient history updates
    // Use static sized type to avoid heap alloc
    // This class is very frequently used in fetch/BP, so ensure it is a fixed-sized object
    std::array<std::size_t, staticMaxShamtLimit> posHighestBitsInGhr;  // Positions of highest bits in global history
    std::array<std::size_t, staticMaxShamtLimit> posHighestBitsInOldFoldedHist;  // Positions in old folded history

    // Perform an immediate fold on given history bitvec
    uint64_t fold(const boost::dynamic_bitset<> &historyBitVec);

  public:
    /**
     * Constructor for FoldedHistBase
     * @param histLen Length of the original branch history
     * @param foldedLen Length of the folded (compressed) history
     * @param maxShamt Maximum number of bits to shift during updates
     */
    FoldedHistBase(int histLen, int foldedLen, int maxShamt)
        : histLen(histLen), foldedLen(foldedLen), maxShamt(maxShamt), _folded(0)
    {
        assert(maxShamt <= staticMaxShamtLimit);
        assert(foldedLen + maxShamt < 64);  // Ensure folded history fits in uint64_t
        for (int i = 0; i < maxShamt; i++) {
            posHighestBitsInGhr[i] = histLen - 1 - i;
            posHighestBitsInOldFoldedHist[i] = (histLen - 1 - i) % foldedLen;
        }
    }

    virtual ~FoldedHistBase() = default;

    /**
     * Get the current folded history as uint64_t
     * @return The folded history bits
     */
    uint64_t get() const { return _folded; }

    /**
     * Get the current folded history as bitset for compatibility
     * @return The folded history as boost::dynamic_bitset
     */
    boost::dynamic_bitset<> getAsBitset() const { return boost::dynamic_bitset<>(foldedLen, _folded); }

    /**
     * Update the folded history with a new branch outcome
     * @param ghr Global history register
     * @param shamt Number of bits to shift
     * @param taken Whether the branch was taken
     * @param pc Branch PC (used only for path history update)
     * @param target Branch target (used only for path history update)
     */
    virtual void update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc = 0, Addr target = 0) = 0;

    /**
     * Recover the folded history from another instance
     * Used during branch misprediction recovery
     * @param other The FoldedHistBase to recover from
     */
    void recover(FoldedHistBase &other);

    /**
     * Verify that the folded history is consistent with the global history
     * @param ghr Global history register to check against
     */
    void check(const boost::dynamic_bitset<> &ghr);
};

/**
 * DirectionFoldedHist: Common implementation for branch direction history types.
 * This class handles the standard folded history update algorithm used for:
 * - Global history (all branches)
 * - Global backward history (backward branches only)
 * - Local history (per-branch history)
 *
 * All three types share identical update logic for tracking branch direction (taken/not taken).
 */
class DirectionFoldedHist : public FoldedHistBase
{
  public:
    DirectionFoldedHist(int histLen, int foldedLen, int maxShamt) : FoldedHistBase(histLen, foldedLen, maxShamt) {}

    void update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc = 0, Addr target = 0) override;
};

// Type aliases for the three direction history types that share the same implementation
using GlobalFoldedHist = DirectionFoldedHist;
using GlobalBwFoldedHist = DirectionFoldedHist;
using LocalFoldedHist = DirectionFoldedHist;

class ImliFoldedHist : public FoldedHistBase
{
  public:
    ImliFoldedHist(int histLen, int foldedLen, int maxShamt) : FoldedHistBase(histLen, foldedLen, maxShamt)
    {
        assert(foldedLen >= histLen);
    }

    void update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc = 0, Addr target = 0) override;
};

class PathFoldedHist : public FoldedHistBase
{
  public:
    PathFoldedHist(int histLen, int foldedLen, int maxShamt) : FoldedHistBase(histLen, foldedLen, maxShamt) {}

    void update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc = 0, Addr target = 0) override;
};

using FoldedHist = FoldedHistBase;

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_BTB_FOLDED_HIST_HH__
