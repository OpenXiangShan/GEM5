#ifndef __CPU_PRED_BTB_FOLDED_HIST_HH__
#define __CPU_PRED_BTB_FOLDED_HIST_HH__

#include <array>

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

/**
 * FoldedHist implements a folded history mechanism for branch prediction.
 * It maintains a compressed version of branch history by XORing multiple history bits
 * into a smaller number of bits, which helps reduce storage while preserving
 * correlation information.
 */
class FoldedHist
{
  private:
    constexpr static int staticMaxShamtLimit = 16;
    int histLen;    // Length of the original history
    int foldedLen;  // Length of the folded (compressed) history
    int maxShamt;   // Maximum shift amount for history updates
    HistoryType type;
    uint64_t folded;  // The folded history bits

    // Pre-calculated positions for efficient history updates
    // Use static sized type to avoid heap alloc
    // This class is very frequently used in fetch/BP, so ensure it is a fixed-sized object
    std::array<std::size_t, staticMaxShamtLimit> posHighestBitsInGhr;  // Positions of highest bits in global history
    std::array<std::size_t, staticMaxShamtLimit> posHighestBitsInOldFoldedHist;  // Positions in old folded history

  public:
    /**
     * Constructor for FoldedHist
     * @param histLen Length of the original branch history
     * @param foldedLen Length of the folded (compressed) history
     * @param maxShamt Maximum number of bits to shift during updates
     */
    FoldedHist(int histLen, int foldedLen, int maxShamt, HistoryType type = HistoryType::GLOBAL)
        : histLen(histLen), foldedLen(foldedLen), maxShamt(maxShamt), type(type), folded(0)
    {
        assert(maxShamt <= staticMaxShamtLimit);
        assert(foldedLen + maxShamt < 64);  // Ensure folded history fits in uint64_t
        for (int i = 0; i < maxShamt; i++) {
            posHighestBitsInGhr[i] = histLen - 1 - i;
            posHighestBitsInOldFoldedHist[i] = (histLen - 1 - i) % foldedLen;
        }
    }

  public:
    /**
     * Get the current folded history as uint64_t
     * @return The folded history bits
     */
    uint64_t get() const { return folded; }

    /**
     * Get the current folded history as bitset for compatibility
     * @return The folded history as boost::dynamic_bitset
     */
    boost::dynamic_bitset<> getAsBitset() const { return boost::dynamic_bitset<>(foldedLen, folded); }

    /**
     * Update the folded history with a new branch outcome
     * @param ghr Global history register
     * @param shamt Number of bits to shift
     * @param taken Whether the branch was taken
     */
    void update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc = 0);

    /**
     * Recover the folded history from another instance
     * @param other The FoldedHist to recover from
     */
    void recover(FoldedHist &other);

    /**
     * Verify that the folded history is consistent with the global history
     * @param ghr Global history register to check against
     */
    void check(const boost::dynamic_bitset<> &ghr);
};

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_BTB_FOLDED_HIST_HH__
