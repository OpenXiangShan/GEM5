#include "cpu/pred/btb/folded_hist.hh"
// #include "debug/MGSC.hh"

#include "base/logging.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

uint64_t
FoldedHistBase::fold(const boost::dynamic_bitset<> &ghr)
{
    // Create ideal folded history from GHR
    uint64_t folded = 0;

    // Create mask for foldedLen bits
    const uint64_t foldedMask = ((1ULL << foldedLen) - 1);

    // Process in chunks of foldedLen bits
    for (size_t startBit = 0; startBit < histLen; startBit += foldedLen) {
        uint64_t chunk = 0;
        size_t chunkSize = std::min(foldedLen, histLen - startBit);

        // Extract chunk from bitset
        for (size_t i = 0; i < chunkSize; i++) {
            chunk |= (ghr[startBit + i] << i);
        }

        // XOR this chunk into the ideal folded history
        folded ^= chunk;
    }

    folded &= foldedMask;

    return folded;
}

/**
 * Recover folded history from another instance.
 * Used during branch misprediction recovery.
 */
void
FoldedHistBase::recover(FoldedHistBase &other)
{
    // Verify both histories have same configuration
    assert(foldedLen == other.foldedLen);
    assert(maxShamt == other.maxShamt);
    assert(histLen == other.histLen);
    // Copy the folded history
    _folded = other._folded;
}

/**
 * Verify that folded history matches with what would be computed from GHR.
 *
 * History folding XORs foldedLen-sized chunks:
 * folded = [foldedLen-1:0] ^ [2*foldedLen-1:foldedLen] ^ [3*foldedLen-1:2*foldedLen] ^ ...
 * This method can be commonly used for checking both GHR and PHR.
 */
void
FoldedHistBase::check(const boost::dynamic_bitset<> &historyBitVec)
{
    // Verify our folded history matches ideal
    auto expected = fold(historyBitVec);
    if (_folded != expected) {
        std::string hist_str;
        boost::to_string(historyBitVec, hist_str);
        warn(
            "FoldedHist check failed: foldedLen=%d, histLen=%d, \
            expected=0x%lx, actual=0x%lx, history=%s\n",
            foldedLen, histLen, expected, _folded, hist_str.c_str());
    }
    assert(_folded == expected);
}

/**
 * Update the folded history with a new branch outcome for direction histories.
 * This implementation is used for Global, Global Backward, and Local histories.
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
DirectionFoldedHist::update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc, Addr target)
{
    assert(shamt >= 0);
    // Create mask for folded length
    const uint64_t foldedMask = ((1ULL << foldedLen) - 1);

    uint64_t temp = _folded;

    // Case 1: shamt >= history length, calculate completely new folded
    if (shamt >= histLen) {
        temp = taken ? 0b1 : 0b0;  // last bit is 1/0 (T/NT), all others are 0
    }
    // Case 2: When folded length >= history length
    else if (foldedLen >= histLen) {
        // Simple shift and set case
        temp <<= shamt;
        // Clear any bits beyond histLen
        temp &= ((1ULL << histLen) - 1);
        // Set the newest bit based on branch outcome
        if (taken) {
            temp |= 1;
        }
    }
    // Case 3: When folded length < history length
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
    _folded = temp;
}

void
ImliFoldedHist::update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc, Addr target)
{
    // IMLI requires folded length >= history length
    assert(foldedLen >= histLen);
    const uint64_t foldedMask = ((1ULL << foldedLen) - 1);
    uint64_t temp = _folded;

    // For IMLI, we treat "taken" as a backward-taken event (i.e., loop continues).
    // Update rule (CBP-like):
    // - backward taken: counter++
    // - otherwise: counter = 0 (loop exits or no backward-taken event)
    if (taken) {
        const uint64_t max = ((1ULL << histLen) - 1);
        if (temp < max) {
            temp++;
        }
    } else {
        temp = 0;
    }
    _folded = temp & foldedMask;
    // DPRINTF(MGSC, "IMLI FoldedHist update: shamt %d, taken %d, folded %ld\n", shamt, taken, _folded);
}

/**
 * Update path history with branch target information.
 * Only updates on taken branches.
 * Uses path hashing instead of branch direction.
 */
void
PathFoldedHist::update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc, Addr target)
{
    if (taken) {
        // Calculate path hash
        uint64_t hash = pathHash(pc, target);

        const uint64_t foldedMask = ((1ULL << foldedLen) - 1);
        uint64_t temp = _folded;

        assert(shamt <= foldedLen);
        assert(shamt <= histLen);

        // Case 1: When folded length >= history length
        if (foldedLen >= histLen) {
            // Simple shift and set case
            temp <<= shamt;
            temp ^= hash;
            // Clear any bits beyond histLen
            temp &= ((1ULL << histLen) - 1);
        }
        // Case 2: When folded length < history length
        else {
            assert(shamt <= maxShamt);
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
            uint64_t effectiveHash = hash;
            if (histLen < pathHashLength) {
                const uint64_t mask = (1ULL << histLen) - 1;
                effectiveHash &= mask;
            }
            temp ^= foldHash(effectiveHash, foldedLen);

            // Mask to folded length
            temp &= foldedMask;
        }
        _folded = temp;
    }
}

SelectableFoldedHist::SelectableFoldedHist()
    : historyType(HistoryType::PATH),
      storage(std::in_place_type<PathFoldedHist>, 1, 1, 1)
{
}

SelectableFoldedHist::SelectableFoldedHist(int histLen, int foldedLen,
                                           int maxShamt,
                                           HistoryType historyType)
    : historyType(historyType),
      storage(std::in_place_type<PathFoldedHist>, 1, 1, 1)
{
    switch (historyType) {
      case HistoryType::PATH:
        storage.emplace<PathFoldedHist>(histLen, foldedLen, maxShamt);
        break;
      case HistoryType::GLOBAL:
        storage.emplace<DirectionFoldedHist>(histLen, foldedLen, maxShamt);
        break;
      default:
        panic("SelectableFoldedHist only supports GLOBAL and PATH, got %d",
              static_cast<int>(historyType));
    }
}

uint64_t
SelectableFoldedHist::get() const
{
    return visit([](const auto &hist) { return hist.get(); });
}

boost::dynamic_bitset<>
SelectableFoldedHist::getAsBitset() const
{
    return visit([](const auto &hist) { return hist.getAsBitset(); });
}

void
SelectableFoldedHist::update(const boost::dynamic_bitset<> &history, int shamt,
                             bool taken, Addr pc, Addr target)
{
    visit([&](auto &hist) { hist.update(history, shamt, taken, pc, target); });
}

void
SelectableFoldedHist::recover(const SelectableFoldedHist &other)
{
    assert(historyType == other.historyType);
    visit([&](auto &hist) {
        using HistType = std::decay_t<decltype(hist)>;
        auto &otherHist =
            const_cast<HistType &>(std::get<HistType>(other.storage));
        hist.recover(otherHist);
    });
}

void
SelectableFoldedHist::check(const boost::dynamic_bitset<> &history) const
{
    visit([&](const auto &hist) {
        auto &mutableHist = const_cast<std::decay_t<decltype(hist)> &>(hist);
        mutableHist.check(history);
    });
}

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
