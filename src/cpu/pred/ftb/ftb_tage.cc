#include "cpu/pred/ftb/ftb_tage.hh"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "base/debug_helper.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "cpu/pred/ftb/stream_common.hh"
#include "debug/DecoupleBP.hh"
#include "debug/DecoupleBPUseful.hh"
#include "debug/DecoupleBPVerbose.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred{

FTBTAGE::FTBTAGE(const Params& p):
TimedPredictor(p),
numPredictors(p.numPredictors),
tableSizes(p.tableSizes),
tableTagBits(p.TTagBitSizes),
tablePcShifts(p.TTagPcShifts),
histLengths(p.histLengths),
maxHistLen(p.maxHistLen),
numTablesToAlloc(p.numTablesToAlloc)
{
    tageTable.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    tableIndexMasks.resize(numPredictors);
    tableTagMasks.resize(numPredictors);
    baseTable.resize(128); // need modify
    for (unsigned int i = 0; i < p.numPredictors; ++i) {
        //initialize ittage predictor
        assert(tableSizes.size() >= numPredictors);
        tageTable[i].resize(tableSizes[i]);

        tableIndexBits[i] = ceilLog2(tableSizes[i]);
        tableIndexMasks[i].resize(maxHistLen, true);
        tableIndexMasks[i] >>= (maxHistLen - tableIndexBits[i]);

        assert(histLengths.size() >= numPredictors);

        assert(tableTagBits.size() >= numPredictors);
        tableTagMasks[i].resize(maxHistLen, true);
        tableTagMasks[i] >>= (maxHistLen - tableTagBits[i]);

        assert(tablePcShifts.size() >= numPredictors);
    }
    useAlt.resize(128, 0);
    usefulResetCnt = 128;
}

void
FTBTAGE::tickStart()
{
    prediction.valid = false;
}

void
FTBTAGE::tick() {}

bool
FTBTAGE::lookupHelper(Addr startAddr,
                      TageEntry &main_entry,
                      int& main_table,
                      int& main_table_index,
                      bool& use_alt_pred, bitset &usefulMask)
{
    main_table = -1;
    main_table_index = -1;

    // make main prediction
    int provider_counts = 0;
    for (int i = numPredictors - 1; i >= 0; --i) {
        Addr tmp_index = getTageIndex(startAddr, i);
        Addr tmp_tag = getTageTag(startAddr, i);
        auto &way = tageTable[i][tmp_index];
        bool match = way.valid && matchTag(tmp_tag, way.tag);

        if (match) {
            main_entry = &way;
            main_table = i;
            main_table_index = tmp_index;
            ++provider_counts;
            break;
        }

        usefulMask <<= 1;
        usefulMask[0] = way.useful;
    }

    if (provider_counts > 0) {
        if (useAlt.find(getUseAltIdx(startAddr)) > 0 &&
            (main_entry.counter == 1 || main_entry.counter == 2)) {
            use_alt_pred = true;
        } else {
            use_alt_pred = false;
        }
        return true;
    } else {
        return false;
    }
}

void
FTBTAGE::putPCHistory(Addr stream_start, const bitset &history, std::array<FullFTBPrediction> &stagePreds) {
    TageEntry entry;
    int main_table = -1;
    int main_table_index = -1;
    bool use_alt_pred = false;
    bitset usefulMask;

    bool found = lookupHelper(stream_start, entry, main_table,
                              main_table_index, use_alt_pred);

    bool altRes = baseTable.at(stream_start % baseTable.size());

    if (!found || use_alt_pred) {
        // use base predictor result.
        TagePrediction pred(found, entry.counter, altRes, main_table, main_table_index, entry.tag, true, usefulMask);
    } else {
        // use main entry result.
        TagePrediction pred(found, entry.counter, altRes, main_table, main_table_index, entry.tag, false, usefulMask);
    }
}

void
FTBTAGE::update(const boost::dynamic_bitset<> &history, Addr startAddr, const TagePrediction pred, bool actualTaken)
{
    bool mainTaken = way.counter >= 2;
    bool altTaken = baseTable.at(startAddr % baseTable.size()) >= 2;

    // update useful bit, counter and predTaken for main entry
    if (pred.mainFound) { // updateProvided
        auto &way = tageTable[pred.table][pred.index];

        if (mainTaken != altTaken) { // updateAltDiffers
            way.useful = actualTaken == mainTaken; // updateProviderCorrect
        }

        updateCounter(actualTaken, mainTaken, way.counter);
    }

    // update base table counter
    if (pred.useAlt)
        updateCounter(actualTaken, altTaken, baseTable.at(startAddr % baseTable.size()));

    // update use_alt_counters
    if (pred.mainFound && pred.counter == 0 && mainTaken != altTaken) {
        auto &use_alt_counter = useAlt.at(getUseAltIdx(startAddr));
        if (altTaken == actualTaken) {
             satIncrement(8, use_alt_counter);
        } else {
             satDecrement(-7, use_alt_counter);
        }
    }

    // update useful reset counter
    bool needToAllocate = ((pred.useAlt && altTaken != actualTaken) || (!pred.useAlt && mainTaken != actualTaken)) &&
                          !(pred.useAlt && pred.mainFound && mainTaken == actualTaken);

    bool incUsefulResetCounter = pred.usefulMask.count() < ((numPredictors - (pred.table + 1)) - pred.usefulMask.count());
    bool decUsefulResetCounter = pred.usefulMask.count() > ((numPredictors - (pred.table + 1)) - pred.usefulMask.count());
    unsigned changeVal = std::abs(pred.usefulMask.count() - ((numPredictors - (pred.table + 1)) - pred.usefulMask.count()));
    if (needToAllocate) {
        if (incUsefulResetCounter) { // need modify: clear the useful bit of all entries
            usefulResetCnt = usefulResetCnt + changeVal >= 128 ? 128 : usefulResetCnt + changeVal;
        } else if (decUsefulResetCounter) {
            usefulResetCnt = usefulResetCnt - changeVal <= 0 ? 0 : usefulResetCnt - changeVal;
        }
    }

    if (usefulResetCnt == 0) {
        for (auto table : tageTable) {
            for (auto entry : table) {
                entry.useful = 0;
            }
        }
    }

    // allocate new entry
    unsigned maskMaxNum = std::pow(2, (numPredictors - (pred.table + 1)));
    srand((unsigned)time(NULL));
    unsigned mask = std::rand() % maskMaxNum;
    bitset allocateLFSR(numPredictors - (pred.table + 1), mask);
    bitset allocate = (allocateLFSR & pred.usefulMask.flip()).any() ? allocateLFSR & pred.usefulMask.flip() : pred.usefulMask.flip();
    short newCounter = actualTaken ? 3 : 0;

    bool allocateValid = pred.usefulMask.flip().any();
    if (needToAllocate && allocateValid) {
        unsigned startTable = pred.table + 1;

        for (int index = startTable;index < numPredictors;index++) {
            Addr newIndex = getTageIndex(startAddr, index);
            Addr newTag = getTageTag(startAddr, index);
            auto &entry = tageTable[newIndex][newTag];

            if (allocate[index - startTable]) {
                entry = TageEntry(startAddr, newTag, newCounter);
            }
        }
    }
}

void
FTBTAGE::updateCounter(bool actualTaken, bool predTaken, short &counter) {
    if (actualTaken) {
        if (actualTaken == predTaken)
            satIncrement(3, counter);
        else
            satDecrement(0, counter);
    } else {
        if (actualTaken != predTaken)
            satIncrement(3, counter);
        else
            satDecrement(0, counter);
    }
}

Addr
FTBTAGE::getTageTag(Addr pc, int t)
{
    bitset buf(tableTagBits[t], pc >> tablePcShifts[t]);  // lower bits of PC
    buf.resize(maxHistLen);
    auto foldedHist = tagFoldedHist[t];
    buf ^= foldedHist;
    return buf.to_ulong();
}

Addr
FTBTAGE::getTageIndex(Addr pc, const bitset& history, int t)
{
    bitset buf(tableIndexBits[t], pc >> tablePcShifts[t]);  // lower bits of PC
    buf.resize(maxHistLen);
    auto foldedHist = tagFoldedHist[t];
    buf ^= foldedHist;
    return buf.to_ulong();
}

bool
FTBTAGE::matchTag(Addr expected, Addr found)
{
    return expected == found;
}

bool
FTBTAGE::satIncrement(int max, short &counter)
{
    if (counter < max) {
        ++counter;
    }
    return counter == max;
}

bool
FTBTAGE::satDecrement(int min, short &counter)
{
    if (counter > min) {
        --counter;
    }
    return counter == min;
}

Addr
FTBTAGE::getUseAltIdx(Addr pc) {
    return pc & (useAlt.size() - 1); // need modify
}

void
FTBTAGE::specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred)
{
    for (int t = 0;t < numPredictors;t++) {
        for (int type = 0;type < 2;type++) {
            auto &foldedHist = type ? tagFoldedHist[t] : indexFoldedHist[t];
            unsigned int foldedLen = type ? tableTagBits[t] : tableIndexBits[t];
            unsigned int modResult = histLengths[t] % foldedLen;

            if (foldedLen < histLengths[t]) {
                bitset tempHist(history);
                tempHist.resize(histLengths[t]);

                bool foldedHighRes = foldedHist[foldedLen - 1];
                bool oriHighRes = tempHist[histLengths[t] - 1];
                bool res = pred.; // need modify

                foldedHist <<= 1;
                foldedHist[0] = res ^ foldedHighRes;
                foldedHist[modResult] ^= oriHighRes;
            }
            foldedHist.resize(foldedLen);
        }
    }
}

void
FTBTAGE::recoverFoldedHist(const bitset& history)
{
    // manually compute the folded history
    for (int t = 1;t < numPredictors;t++) {
        for (int type = 0;type < 2;type++) {
            auto &foldedHist = type ? tagFoldedHist[t] : indexFoldedHist[t];
            unsigned int foldedLen = type ? tableTagBits[t] : tableIndexBits[t];
            foldedHist.clear();
            foldedHist.resize(foldedLen);
            bitset tempHist(history);
            tempHist.resize(histLengths[t]);
            for (int j = 0;j < histLengths[t];j++) {
                foldedHist[j % foldedLen] ^= tempHist[j];
            }
        }
    }
}

} // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
