/*
 * Tree-PLRU (Pseudo-LRU) Implementation
 *
 * Based on rocket-chip's PseudoLRU implementation:
 * https://github.com/chipsalliance/rocket-chip/blob/master/src/main/scala/util/Replacement.scala
 *
 * Binary tree structure for 4-way:
 *                  bit[2]: ways 3+2 older than ways 1+0
 *                  /                                  \
 *     bit[1]: way 3 older than way 2    bit[0]: way 1 older than way 0
 *
 * For 8-way:
 *                      bit[6]: ways 7-4 older than ways 3-0
 *                      /                                  \
 *            bit[5]: ways 7+6 > 5+4                bit[2]: ways 3+2 > 1+0
 *            /                    \                /                    \
 *     bit[4]: way 7>6    bit[3]: way 5>4    bit[1]: way 3>2    bit[0]: way 1>0
 */

#ifndef __CPU_PRED_BTB_TREE_PLRU_HH__
#define __CPU_PRED_BTB_TREE_PLRU_HH__

#include <cassert>
#include <cstdint>
#include <vector>

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

#ifdef UNIT_TEST
namespace test {
#endif

class TreePLRU
{
  public:
    TreePLRU(unsigned numSets, unsigned numWays);

    // Touch a way (mark as recently used)
    void touch(unsigned setIdx, unsigned way);

    // Touch multiple ways in order (for predict path)
    void touchMultiple(unsigned setIdx, const std::vector<unsigned>& ways);

    // Get the victim way for replacement
    unsigned getVictim(unsigned setIdx) const;

    // Get number of state bits per set
    unsigned getStateBits() const { return numWays - 1; }

  private:
    // Recursive helper to compute next state after touching a way
    uint8_t getNextState(uint8_t state, unsigned touchWay, unsigned treeNways) const;

    // Recursive helper to find replacement way
    unsigned getReplaceWay(uint8_t state, unsigned treeNways) const;

    // Helper to get log2 ceiling
    static unsigned log2Ceil(unsigned n);

    unsigned numSets;
    unsigned numWays;

    // State storage: one state per set, each state is (numWays-1) bits
    std::vector<uint8_t> states;
};

#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_TREE_PLRU_HH__
