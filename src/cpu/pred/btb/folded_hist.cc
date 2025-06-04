#include "cpu/pred/btb/folded_hist.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

/**
 * Update the folded history with a new branch outcome.
 * Note: pc is used only for path history update!
 *
 * Example:
 * If we have:
 *   histLen = 8 (original history length)
 *   foldedLen = 4 (compressed history length)
 *   maxShamt = 2 (maximum shift amount)
 *
 * Case 1: foldedLen >= histLen (e.g., histLen=4, foldedLen=8)
 * - Simply shift and set new bit
 *
 * Case 2: foldedLen < histLen (e.g., histLen=8, foldedLen=4)
 * - XOR the highest bits that would be lost in shift
 * - Then shift and set new bit
 */
void
FoldedHist::update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc)
{
    // Create mask for folded length
    const uint64_t foldedMask = ((1ULL << foldedLen) - 1);

    if (type == HistoryType::GLOBAL || type == HistoryType::GLOBALBW || type == HistoryType::LOCAL) {
        uint64_t temp = folded;

        // Case 1: When folded length >= history length
        if (foldedLen >= histLen) {
            // Simple shift and set case
            temp <<= shamt;
            // Clear any bits beyond histLen
            temp &= ((1ULL << histLen) - 1);
            // Set the newest bit based on branch outcome
            if (taken) {
                temp |= 1;
            }
        }
        // Case 2: When folded length < history length
        else {
            // Step 1: Handle the bits that would be lost in shift
            for (int i = 0; i < shamt; i++) {
                // XOR the highest bits from GHR with corresponding positions in folded history
                temp ^= (ghr[posHighestBitsInGhr[i]] << posHighestBitsInOldFoldedHist[i]);
            }

            // Step 2: Perform the shift
            temp <<= shamt;

            // Step 3: Copy the XORed bits back to lower positions
            for (int i = 0; i < shamt; i++) {
                uint64_t highBit = (temp >> (foldedLen + i)) & 1;
                temp |= (highBit << i);
            }

            // Step 4: Add new branch outcome
            if (taken) {
                temp ^= 1;
            }

            // Step 5: Mask to folded length
            temp &= foldedMask;
        }
        folded = temp;
    } else if (type == HistoryType::IMLI) {
        // Case 1: When folded length >= history length
        assert(foldedLen >= histLen);  // Requirement of IMLI
        uint64_t temp = folded;
        // Simple shift and set case
        if (taken && temp < ((1ULL << histLen) - 1) && shamt == 1) {  // backward taken, inner most loop
            temp = temp + 1;                                          // counter++ (index++)
        } else if (taken && shamt > 1) {                              // backward taken, not inner most loop
            temp = 1;
        } else if (!taken) {  // backward not taken, hist = 0
            temp = 0;
        }
        folded = temp & foldedMask;
    } else if (type == HistoryType::PATH) {
        if (taken) {
            uint64_t temp = folded;
            // Case 1: When folded length >= history length
            if (foldedLen >= histLen) {
                // Simple shift and set case
                temp <<= 2;
                // Clear any bits beyond histLen
                temp &= ((1ULL << histLen) - 1);
                // Set the newest bits based on PC
                uint64_t pcBits = ((pc >> 1) ^ (pc >> 3) ^ (pc >> 5) ^ (pc >> 7));
                temp |= (pcBits & 0b11);
            }
            // Case 2: When folded length < history length
            else {
                assert(shamt == 1);
                assert(maxShamt >= 2);
                // First shift for bit 1
                // Step 1: Handle the bits that would be lost in shift
                temp ^= (ghr[posHighestBitsInGhr[0]] << posHighestBitsInOldFoldedHist[0]);

                // Step 2: Perform the shift
                temp <<= 1;

                // Step 3: Copy the XORed bit back to lower position
                uint64_t highBit = (temp >> foldedLen) & 1;
                temp |= highBit;

                // Step 4: Add new branch outcome (bit 1 of PC hash)
                uint64_t pcBit1 = (((pc >> 1) ^ (pc >> 3) ^ (pc >> 5) ^ (pc >> 7)) >> 1) & 1;
                temp ^= pcBit1;

                // Mask to folded length
                temp &= foldedMask;

                // Second shift for bit 0
                // Step 1: Handle the bits that would be lost in shift
                temp ^= (ghr[posHighestBitsInGhr[0]] << posHighestBitsInOldFoldedHist[1]);

                // Step 2: Perform the shift
                temp <<= 1;

                // Step 3: Copy the XORed bit back to lower position
                highBit = (temp >> foldedLen) & 1;
                temp |= highBit;


                // Step 4: Add new branch outcome (bit 0 of PC hash)
                uint64_t pcBit0 = ((pc >> 1) ^ (pc >> 3) ^ (pc >> 5) ^ (pc >> 7)) & 1;
                temp ^= pcBit0;

                // Mask to folded length
                temp &= foldedMask;
            }
            folded = temp;
        }
    }
}

/**
 * Recover folded history from another instance.
 * Used during branch misprediction recovery.
 */
void
FoldedHist::recover(FoldedHist &other)
{
    // Verify both histories have same configuration
    assert(foldedLen == other.foldedLen);
    assert(maxShamt == other.maxShamt);
    assert(histLen == other.histLen);
    // Copy the folded history
    folded = other.folded;
}

/**
 * Verify that folded history matches what would be computed from GHR.
 *
 * History folding XORs foldedLen-sized chunks:
 * folded = [foldedLen-1:0] ^ [2*foldedLen-1:foldedLen] ^ [3*foldedLen-1:2*foldedLen] ^ ...
 */
void
FoldedHist::check(const boost::dynamic_bitset<> &ghr)
{
    // Create ideal folded history from GHR
    uint64_t idealFolded = 0;

    // Create mask for foldedLen bits
    const uint64_t foldedMask = ((1ULL << foldedLen) - 1);

    // Process in chunks of foldedLen bits
    for (size_t startBit = 0; startBit < histLen; startBit += foldedLen) {
        uint64_t chunk = 0;
        size_t chunkSize = std::min(static_cast<size_t>(foldedLen), histLen - startBit);

        // Extract chunk from bitset
        for (size_t i = 0; i < chunkSize; i++) {
            chunk |= (ghr[startBit + i] << i);
        }

        // XOR this chunk into the ideal folded history
        idealFolded ^= chunk;
    }

    idealFolded &= foldedMask;

    // Verify our folded history matches ideal
    assert(idealFolded == folded);
}

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
