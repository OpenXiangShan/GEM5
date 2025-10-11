#include "cpu/pred/btb/folded_hist.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

uint64_t
FoldedHistBase::fold(const boost::dynamic_bitset<> &ghr)
{
    uint64_t folded = 0;

    const uint64_t foldedMask = ((1ULL << foldedLen) - 1);

    for (size_t startBit = 0; startBit < histLen; startBit += foldedLen) {
        uint64_t chunk = 0;
        size_t chunkSize = std::min(foldedLen, histLen - startBit);

        for (size_t i = 0; i < chunkSize; i++) {
            chunk |= (ghr[startBit + i] << i);
        }

        folded ^= chunk;
    }

    folded &= foldedMask;

    return folded;
}

void
FoldedHistBase::recover(FoldedHistBase &other)
{
    assert(foldedLen == other.foldedLen);
    assert(maxShamt == other.maxShamt);
    assert(histLen == other.histLen);
    _folded = other._folded;
}

void
FoldedHistBase::check(const boost::dynamic_bitset<> &historyBitVec)
{
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

void
DirectionFoldedHist::update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc, Addr target)
{
    assert(shamt >= 0);
    const uint64_t foldedMask = ((1ULL << foldedLen) - 1);

    uint64_t temp = _folded;

    if (shamt >= histLen) {
        _folded = fold(ghr);
    } else if (foldedLen >= histLen) {
        temp <<= shamt;
        temp &= ((1ULL << histLen) - 1);
        if (taken) {
            temp |= 1;
        }
    } else {
        for (int i = 0; i < shamt; i++) {
            temp ^= (ghr[posHighestBitsInGhr[i]] << posHighestBitsInOldFoldedHist[i]);
        }

        temp <<= shamt;

        for (int i = 0; i < shamt; i++) {
            uint64_t highBit = (temp >> (foldedLen + i)) & 1;
            temp |= (highBit << i);
        }

        if (taken) {
            temp ^= 1;
        }

        temp &= foldedMask;
    }
    _folded = temp;
}

void
ImliFoldedHist::update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc, Addr target)
{
    assert(foldedLen >= histLen);
    const uint64_t foldedMask = ((1ULL << foldedLen) - 1);
    uint64_t temp = _folded;

    if (taken && temp < ((1ULL << histLen) - 1) && shamt == 1) {
        temp = temp + 1;
    } else if (taken && shamt > 1) {
        temp = 1;
    } else if (!taken) {
        temp = 0;
    }
    _folded = temp & foldedMask;
}

void
PathFoldedHist::update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken, Addr pc, Addr target)
{
    if (taken) {
        uint64_t hash = pathHash(pc, target);

        const uint64_t foldedMask = ((1ULL << foldedLen) - 1);
        uint64_t temp = _folded;

        if (foldedLen >= histLen) {
            temp <<= shamt;
            temp ^= hash;
            temp &= ((1ULL << histLen) - 1);
        } else {
            assert(shamt <= maxShamt);
            for (int i = 0; i < shamt; i++) {
                temp ^= (ghr[posHighestBitsInGhr[i]] << posHighestBitsInOldFoldedHist[i]);
            }

            temp <<= shamt;

            for (int i = 0; i < shamt; i++) {
                uint64_t highBit = (temp >> (foldedLen + i)) & 1;
                temp |= (highBit << i);
            }

            uint64_t effectiveHash = hash;
            if (histLen < pathHashLength) {
                const uint64_t mask = (1ULL << histLen) - 1;
                effectiveHash &= mask;
            }
            temp ^= foldHash(effectiveHash, foldedLen);

            temp &= foldedMask;
        }
        _folded = temp;
    }
}

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
