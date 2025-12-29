/*
 * Tree-PLRU Implementation
 * Matches RTL behavior in XiangShan's PlruStateGen.scala
 *
 * State layout for 4-way (3 bits): Cat(root, left_subtree, right_subtree)
 *   bit[2] = root: 1 means left subtree is older
 *   bit[1] = left subtree: 1 means way3 is older than way2
 *   bit[0] = right subtree: 1 means way1 is older than way0
 *
 * Way encoding: way[msb] selects subtree (0=right, 1=left)
 */

#include "cpu/pred/btb/tree_plru.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

#ifdef UNIT_TEST
namespace test {
#endif

TreePLRU::TreePLRU(unsigned numSets, unsigned numWays)
    : numSets(numSets), numWays(numWays)
{
    assert(numWays >= 2 && "TreePLRU requires at least 2 ways");
    assert(numWays <= 8 && "TreePLRU supports up to 8 ways");
    states.resize(numSets, 0);
}

unsigned
TreePLRU::log2Ceil(unsigned n)
{
    unsigned result = 0;
    unsigned temp = n - 1;
    while (temp > 0) {
        temp >>= 1;
        result++;
    }
    return result;
}

void
TreePLRU::touch(unsigned setIdx, unsigned way)
{
    assert(setIdx < numSets);
    assert(way < numWays);
    states[setIdx] = getNextState(states[setIdx], way, numWays);
}

void
TreePLRU::touchMultiple(unsigned setIdx, const std::vector<unsigned>& ways)
{
    for (unsigned way : ways) {
        touch(setIdx, way);
    }
}

unsigned
TreePLRU::getVictim(unsigned setIdx) const
{
    assert(setIdx < numSets);
    return getReplaceWay(states[setIdx], numWays);
}

/*
 * Compute next PLRU state after touching a way
 * This matches RTL's getNextState function in PlruStateGen.scala
 *
 * State layout: Cat(root_bit, left_subtree_bits, right_subtree_bits)
 * For 4-way: state[2]=root, state[1]=left, state[0]=right
 * For 8-way: state[6]=root, state[5:3]=left(4-way), state[2:0]=right(4-way)
 */
uint8_t
TreePLRU::getNextState(uint8_t state, unsigned touchWay, unsigned treeNways) const
{
    if (treeNways > 2) {
        unsigned rightNways = 1u << (log2Ceil(treeNways) - 1);
        unsigned leftNways = treeNways - rightNways;

        // setLeftOlder = !touchWay[msb]
        // If touching right subtree (msb=0), set left as older (setLeftOlder=1)
        // If touching left subtree (msb=1), set right as older (setLeftOlder=0)
        bool touchingLeftSubtree = (touchWay >> (log2Ceil(treeNways) - 1)) & 1;
        bool setLeftOlder = !touchingLeftSubtree;

        // Extract subtree states from current state
        // State layout: [root_bit | left_subtree_bits | right_subtree_bits]
        unsigned rightStateBits = (rightNways > 1) ? (rightNways - 1) : 0;
        unsigned leftStateBits = (leftNways > 1) ? (leftNways - 1) : 0;

        uint8_t rightSubtreeState = 0;
        uint8_t leftSubtreeState = 0;

        if (rightNways > 1) {
            rightSubtreeState = state & ((1u << rightStateBits) - 1);
        }
        if (leftNways > 1) {
            leftSubtreeState = (state >> rightStateBits) & ((1u << leftStateBits) - 1);
        }

        // Compute new subtree states
        uint8_t newLeftState = leftSubtreeState;
        uint8_t newRightState = rightSubtreeState;

        if (touchingLeftSubtree && leftNways > 1) {
            // Recurse into left subtree
            unsigned leftTouchWay = touchWay & ((1u << log2Ceil(leftNways)) - 1);
            newLeftState = getNextState(leftSubtreeState, leftTouchWay, leftNways);
        }

        if (!touchingLeftSubtree && rightNways > 1) {
            // Recurse into right subtree
            unsigned rightTouchWay = touchWay & ((1u << log2Ceil(rightNways)) - 1);
            newRightState = getNextState(rightSubtreeState, rightTouchWay, rightNways);
        }

        // Reconstruct state: Cat(setLeftOlder, newLeftState, newRightState)
        uint8_t result = 0;
        if (rightNways > 1) {
            result |= newRightState;
        }
        if (leftNways > 1) {
            result |= (newLeftState << rightStateBits);
        }
        result |= ((setLeftOlder ? 1 : 0) << (rightStateBits + leftStateBits));

        return result;

    } else if (treeNways == 2) {
        // Leaf node: set bit opposite of touchWay[0]
        // If touching way0, set state=1 (way1 is older)
        // If touching way1, set state=0 (way0 is older)
        return (touchWay & 1) ? 0 : 1;
    } else {
        return 0;
    }
}

/*
 * Find the way to replace based on current PLRU state
 * This matches RTL's getReplaceWay function in PlruStateGen.scala
 */
unsigned
TreePLRU::getReplaceWay(uint8_t state, unsigned treeNways) const
{
    if (treeNways > 2) {
        unsigned rightNways = 1u << (log2Ceil(treeNways) - 1);
        unsigned leftNways = treeNways - rightNways;

        unsigned rightStateBits = (rightNways > 1) ? (rightNways - 1) : 0;
        unsigned leftStateBits = (leftNways > 1) ? (leftNways - 1) : 0;

        // leftSubtreeOlder = state[msb]
        bool leftSubtreeOlder = (state >> (rightStateBits + leftStateBits)) & 1;

        // Extract subtree states
        uint8_t rightSubtreeState = 0;
        uint8_t leftSubtreeState = 0;

        if (rightNways > 1) {
            rightSubtreeState = state & ((1u << rightStateBits) - 1);
        }
        if (leftNways > 1) {
            leftSubtreeState = (state >> rightStateBits) & ((1u << leftStateBits) - 1);
        }

        // Recurse into older subtree
        unsigned subWay;
        if (leftSubtreeOlder) {
            if (leftNways > 1) {
                subWay = getReplaceWay(leftSubtreeState, leftNways);
            } else {
                subWay = 0;
            }
            // Return way in left subtree: set msb to 1
            return (1u << (log2Ceil(treeNways) - 1)) | subWay;
        } else {
            if (rightNways > 1) {
                subWay = getReplaceWay(rightSubtreeState, rightNways);
            } else {
                subWay = 0;
            }
            // Return way in right subtree: msb is 0
            return subWay;
        }

    } else if (treeNways == 2) {
        // Leaf node: return the state bit (0 or 1)
        return state & 1;
    } else {
        return 0;
    }
}

#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
