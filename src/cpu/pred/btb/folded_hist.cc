#include "cpu/pred/btb/folded_hist.hh"

namespace gem5 {

namespace branch_prediction {

namespace btb_pred {

/**
 * Update the folded history with a new branch outcome.
 * 
 * Example:
 * If we have:
 *   histLen = 8 (original history length)
 *   foldedLen = 4 (compressed history length)
 *   maxShamt = 2 (maximum shift amount)
 * 
 * Case 1: foldedLen >= histLen (e.g., histLen=4, foldedLen=8)
 * - Simply shift and set new bit
 * Original:  [0,0,0,1]
 * After shift 1 and taken: [0,0,0,0,0,0,1,1]
 * 
 * Case 2: foldedLen < histLen (e.g., histLen=8, foldedLen=4)
 * - XOR the highest bits that would be lost in shift
 * - Then shift and set new bit
 * Original GHR:     [1,1,0,0,1,0,1,0]
 * Folded (4 bits):  [0,1,1,0]
 * After XOR & shift: [1,0,1,1]
 */
void
FoldedHist::update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc)
{
    // Create temporary bitset for manipulation
    if(type == HistoryType::GLOBAL || type == HistoryType::GLOBALBW || type == HistoryType::LOCAL){
        boost::dynamic_bitset<> temp(folded);
        // Case 1: When folded length >= history length
        if (foldedLen >= histLen) {
            // Simple shift and set case
            temp <<= shamt;
            // Clear any bits beyond histLen
            for (int i = histLen; i < foldedLen; i++) {
                temp[i] = 0;
            }
            // Set the newest bit based on branch outcome
            temp[0] = taken;
        }
        // Case 2: When folded length < history length
        else {
            // Step 1: Handle the bits that would be lost in shift
            temp.resize(foldedLen + shamt);
            for (int i = 0; i < shamt; i++) {
                // XOR the highest bits from GHR with corresponding positions in folded history
                temp[posHighestBitsInOldFoldedHist[i]] ^= ghr[posHighestBitsInGhr[i]];
            }

            // Step 2: Perform the shift
            temp <<= shamt;

            // Step 3: Copy the XORed bits back to lower positions
            for (int i = 0; i < shamt; i ++) {
                temp[i] = temp[foldedLen + i];
            }

            // Step 4: Add new branch outcome
            temp[0] ^= taken;

            // Step 5: Restore original size
            temp.resize(foldedLen);
        }
        folded = temp;
    }else if(type == HistoryType::IMLI){
        // Case 1: When folded length >= history length
        assert(foldedLen >= histLen); //TODO
        unsigned temp = folded.to_ulong();
        if (foldedLen >= histLen) {
            // Simple shift and set case
            if(taken && temp < (pow(2, histLen)-1) && shamt == 1){
                temp = temp + 1;
            }else if(taken && shamt > 1){
                temp = 1;
            }else if(!taken){
                temp = 0;
            }
        }
        boost::dynamic_bitset<> temp1(temp);
        temp1.resize(foldedLen);
        folded = temp1;
    }else if(type == HistoryType::PATH){
        if(taken){
            boost::dynamic_bitset<> temp(folded);
            // Case 1: When folded length >= history length
            if (foldedLen >= histLen) {
                // Simple shift and set case
                temp <<= 2;
                // Clear any bits beyond histLen
                for (int i = histLen; i < foldedLen; i++) {
                    temp[i] = 0;
                }
                // Set the newest bit based on branch outcome
                temp[0] = (((pc>>1)^(pc>>3)^(pc>>5)^(pc>>7)) & 1);
                temp[1] = (((pc>>1)^(pc>>3)^(pc>>5)^(pc>>7)) & 2) >> 1;
            }
            // Case 2: When folded length < history length
            else {
                // Step 1: Handle the bits that would be lost in shift
                temp.resize(foldedLen + 1);
                for (int i = 0; i < 1; i++) {
                    // XOR the highest bits from GHR with corresponding positions in folded history
                    temp[posHighestBitsInOldFoldedHist[i]] ^= ghr[posHighestBitsInGhr[i]];
                }

                // Step 2: Perform the shift
                temp <<= 1;

                // Step 3: Copy the XORed bits back to lower positions
                for (int i = 0; i < 1; i ++) {
                    temp[i] = temp[foldedLen + i];
                }

                // Step 4: Add new branch outcome
                temp[0] ^= ((((pc>>1)^(pc>>3)^(pc>>5)^(pc>>7)) & 2) >> 1);

                // Step 5: Restore original size
                temp.resize(foldedLen);

                // Step 1: Handle the bits that would be lost in shift
                temp.resize(foldedLen + 1);
                for (int i = 0; i < 1; i++) {
                    // XOR the highest bits from GHR with corresponding positions in folded history
                    temp[posHighestBitsInOldFoldedHist[i]] ^= ghr[posHighestBitsInGhr[i]];
                }

                // Step 2: Perform the shift
                temp <<= 1;

                // Step 3: Copy the XORed bits back to lower positions
                for (int i = 0; i < 1; i ++) {
                    temp[i] = temp[foldedLen + i];
                }

                // Step 4: Add new branch outcome
                temp[0] ^= (((pc>>1)^(pc>>3)^(pc>>5)^(pc>>7)) & 1);

                // Step 5: Restore original size
                temp.resize(foldedLen);
            }
            folded = temp;
        }
    }

}

/**
 * Recover folded history from another instance.
 * Used during branch misprediction recovery.
 * 
 * Example:
 * hist1: [1,0,1,1]
 * hist2: [0,0,0,0]
 * After hist2.recover(hist1): hist2 = [1,0,1,1]
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
 * Example:
 * pos:             [0,1,2,3,4,5,6,7]
 * GHR (8 bits):    [1,1,0,0,1,0,1,0]
 * pos 0  ^ pos 4 = 1 ^ 1 = 0 -> pos 0
 * pos 1  ^ pos 5 = 1 ^ 0 = 1 -> pos 1
 * pos 2  ^ pos 6 = 0 ^ 1 = 1 -> pos 2
 * pos 3  ^ pos 7 = 0 ^ 0 = 0 -> pos 3
 * Folded (4 bits): [0,1,1,0]
 * 
 * Verification process:
 * 1. Take each bit from GHR
 * 2. XOR into corresponding position in ideal folded history
 * 3. Compare with actual folded history
 */
void
FoldedHist::check(const boost::dynamic_bitset<> &ghr)
{
    // Create ideal folded history from GHR
    boost::dynamic_bitset<> ideal(ghr);
    boost::dynamic_bitset<> idealFolded;
    
    // Resize to match history length
    ideal.resize(histLen);
    idealFolded.resize(foldedLen);
    
    // Fold the history by XORing bits
    for (int i = 0; i < histLen; i++) {
        idealFolded[i % foldedLen] ^= ideal[i];
    }
    
    // Verify our folded history matches ideal
    assert(idealFolded == folded);
}

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5