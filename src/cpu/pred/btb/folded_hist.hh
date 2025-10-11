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

class FoldedHistBase
{
  protected:
    constexpr static int staticMaxShamtLimit = 16;
    std::size_t histLen;
    std::size_t foldedLen;
    std::size_t maxShamt;
    uint64_t _folded;

    std::array<std::size_t, staticMaxShamtLimit> posHighestBitsInGhr;
    std::array<std::size_t, staticMaxShamtLimit> posHighestBitsInOldFoldedHist;

    uint64_t fold(const boost::dynamic_bitset<> &historyBitVec);

  public:
    FoldedHistBase(int histLen, int foldedLen, int maxShamt)
        : histLen(histLen), foldedLen(foldedLen), maxShamt(maxShamt), _folded(0)
    {
        assert(maxShamt <= staticMaxShamtLimit);
        assert(foldedLen + maxShamt < 64);
        for (int i = 0; i < maxShamt; i++) {
            posHighestBitsInGhr[i] = histLen - 1 - i;
            posHighestBitsInOldFoldedHist[i] = (histLen - 1 - i) % foldedLen;
        }
    }

    virtual ~FoldedHistBase() = default;

    uint64_t get() const { return _folded; }

    boost::dynamic_bitset<> getAsBitset() const { return boost::dynamic_bitset<>(foldedLen, _folded); }

    virtual void update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc = 0, Addr target = 0) = 0;

    void recover(FoldedHistBase &other);

    void check(const boost::dynamic_bitset<> &ghr);
};

// Common implementation for branch direction history types
// (global, global backward, and local all share the same update logic)
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
