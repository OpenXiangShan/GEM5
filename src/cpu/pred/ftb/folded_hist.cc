#include "cpu/pred/ftb/folded_hist.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred {

void
FoldedHist::update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken)
{
    // Update the folded history
    boost::dynamic_bitset<> temp(folded);
    if (foldedLen >= histLen) {
        temp <<= shamt;
        for (int i = histLen; i < foldedLen; i++) {
            temp[i] = 0;
        }
        temp[0] = taken;
    } else {
        temp.resize(foldedLen + shamt);
        for (int i = 0; i < shamt; i++) {
            temp[posHighestBitsInOldFoldedHist[i]] ^= ghr[posHighestBitsInGhr[i]];
        }
        temp <<= shamt;
        for (int i = 0; i < shamt; i ++) {
            temp[i] = temp[foldedLen + i];
        }
        temp[0] ^= taken;
        temp.resize(foldedLen);
    }
    folded = temp;
}

void
FoldedHist::recover(FoldedHist &other)
{
    assert(foldedLen == other.foldedLen);
    assert(maxShamt == other.maxShamt);
    assert(histLen == other.histLen);
    folded = other.folded;
}

void
FoldedHist::check(const boost::dynamic_bitset<> &ghr)
{
#ifdef DEBUG
    // Check the folded history now, derive from ghr
    boost::dynamic_bitset<> ideal(ghr);
    boost::dynamic_bitset<> idealFolded;
    ideal.resize(histLen);
    idealFolded.resize(foldedLen);
    for (int i = 0; i < histLen; i++) {
        idealFolded[i % foldedLen] ^= ideal[i];
    }
    assert(idealFolded == folded);
#endif
}

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5