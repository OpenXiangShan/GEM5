#include "cpu/addresspred/enhanced_stride_ap.hh"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

#include "base/logging.hh"
#include "base/random.hh"

namespace gem5
{

namespace addresspred
{

namespace
{

uint64_t
bitMask(int bits)
{
    if (bits <= 0) {
        return 0;
    }
    if (bits >= 64) {
        return std::numeric_limits<uint64_t>::max();
    }
    return (1ull << bits) - 1;
}

} // namespace

EStrideAP::InflightWindow::InflightWindow(int windowTagLength, bool idealWindow)
    : windowTagLength(windowTagLength)
{
    if (idealWindow) {
        hashMethod = [](Addr x, int targetLength) -> uint64_t { return x; };
    } else {
        hashMethod = [](Addr x, int targetLength) -> uint64_t {
            x ^= (x >> 33);
            x *= 0xff51afd7ed558ccd;
            x ^= (x >> 33);
            x *= 0xc4ceb9fe1a85ec53;
            x ^= (x >> 33);
            return x & bitMask(targetLength);
        };
    }
}

int
EStrideAP::InflightWindow::addToInflightWindow(Addr pc)
{
    uint64_t key = hashMethod(pc, windowTagLength);
    return windows[key]++;
}

void
EStrideAP::InflightWindow::removeFromWindow(Addr pc, uint64_t seq_no)
{
    if (seq_no <= lastSeqNo) {
        return;
    }

    uint64_t key = hashMethod(pc, windowTagLength);
    auto it = windows.find(key);
    if (it == windows.end() || it->second <= 0) {
        return;
    }

    --(it->second);
    if (it->second == 0) {
        windows.erase(it);
    }
}

void
EStrideAP::InflightWindow::squash(uint64_t seq_no)
{
    windows.clear();
    lastSeqNo = seq_no;
}

EStrideAP::EStrideAP(const Params &params)
    : APUnit(params),
      ways(params.ways),
      strideWidth(params.strideWidth),
      tagWidth(params.tagWidth),
      logESTBEntrys(params.logESTBEntrys),
      entryCounts(1 << logESTBEntrys),
      logMaxConfidence(params.logMaxConfidence),
      MAXCONFIDENCE(1 << logMaxConfidence),
      confidenceThreshold(static_cast<int>(params.thresholdPercent * MAXCONFIDENCE)),
      dcacheCounterBits(params.dcacheCounterBits),
      dcacheThresholdPercent(params.dcacheThresholdPercent),
      inflightWindow(params.inflightWindowTagLength, params.idealWindow)
{
    gem5_assert(ways > 0, "EStrideAP ways must > 0\n");
    gem5_assert(strideWidth > 0, "EStrideAP strideWidth must > 0\n");
    gem5_assert(strideWidth < 64, "EStrideAP strideWidth must < 64\n");
    gem5_assert(tagWidth > 0, "EStrideAP tagWidth must > 0\n");
    gem5_assert(logESTBEntrys > 0, "EStrideAP logESTBEntrys must > 0\n");
    gem5_assert(logMaxConfidence > 0, "EStrideAP logMaxConfidence must > 0\n");
    gem5_assert(dcacheCounterBits > 0 && dcacheCounterBits <= 8,
                "EStrideAP dcacheCounterBits must be in [1, 8]\n");
    gem5_assert(dcacheThresholdPercent >= 0.0 && dcacheThresholdPercent <= 1.0,
                "EStrideAP dcacheThresholdPercent must be in [0.0, 1.0]\n");
    gem5_assert(params.inflightWindowTagLength > 0,
                "EStrideAP inflightWindowTagLength must > 0\n");

    ESTables.resize(ways);
    for (auto &table : ESTables) {
        table.assign(entryCounts, ESEntry(dcacheCounterBits));
    }
}

int64_t
EStrideAP::extendStride(int64_t entryStride)
{
    const uint64_t mask = bitMask(strideWidth);
    entryStride &= mask;

    const int64_t signBit = 1ull << (strideWidth - 1);
    if (entryStride & signBit) {
        entryStride |= (~mask);
    } else {
        entryStride &= mask;
    }

    return entryStride;
}

uint32_t
EStrideAP::pcHashToWayIndex(Addr pc, int way)
{
    uint64_t hash = pc;
    for (int k = 1; k <= ways; k++) {
        int shift = ((k * logESTBEntrys) - way) % 64;
        if (shift < 0) {
            shift += 64;
        }
        hash ^= (pc >> shift);
    }
    return hash & bitMask(logESTBEntrys);
}

uint32_t
EStrideAP::pcHashToTag(Addr pc, int way)
{
    int j = ways - way;
    if (j < 0) {
        j = 0;
    }

    uint64_t hash = pc;
    for (int k = 1; k <= ways + 1; k++) {
        int shift = ((k * logESTBEntrys) - j) % 64;
        if (shift < 0) {
            shift += 64;
        }
        hash ^= (pc >> shift);
    }
    return hash & bitMask(tagWidth);
}

uint32_t
EStrideAP::compareTags(uint32_t tag1, uint32_t tag2)
{
    return ((tag1 & bitMask(tagWidth)) ^ (tag2 & bitMask(tagWidth)));
}

EStrideAP::UpdateConfDecision
EStrideAP::decideToUpdate(int64_t stride)
{
    auto tryUpdateOnce = []() -> bool {
        return true;
    };

    bool shouldUpdate = tryUpdateOnce();
    const int64_t strideAbs = std::abs(stride);
    if (strideAbs >= 8) {
        shouldUpdate |= tryUpdateOnce();
    }
    if (strideAbs >= 64) {
        shouldUpdate |= tryUpdateOnce();
    }

    bool finalUpdate = shouldUpdate &&
            ((strideAbs > 1) ||
             ((stride == -1) && ((random_mt.random<int32_t>() & 1) == 0)) ||
             ((stride == 1) && ((random_mt.random<int32_t>() & 3) == 0)));
    return {finalUpdate, 1};
}

uint32_t
EStrideAP::tryDecUseful(const ESEntry &entry)
{
    uint32_t k = 2 + 2 * (entry.confidence > MAXCONFIDENCE / 8) +
                 2 * (entry.confidence >= MAXCONFIDENCE / 4);
    uint32_t mask = (1u << k) - 1;
    return random_mt.random<uint32_t>() & mask;
}

bool
EStrideAP::isDcacheConfidenceLow(const ESEntry &entry) const
{
    return entry.dcacheConfidence.calcSaturation() < dcacheThresholdPercent;
}

APResult
EStrideAP::doPredict(APPredMetaData *predMetaData, int inflights)
{
    std::vector<uint32_t> indexEachWays(ways);
    for (size_t i = 0; i < indexEachWays.size(); i++) {
        indexEachWays[i] = pcHashToWayIndex(predMetaData->pc, i);
    }

    std::vector<uint32_t> tagEachWays(ways);
    for (size_t i = 0; i < tagEachWays.size(); i++) {
        tagEachWays[i] = pcHashToTag(predMetaData->pc, i);
    }

    bool found = false;
    int way = 0;
    uint32_t index = 0;
    ESEntry entryCopy;
    for (int i = 0; i < ways; ++i) {
        const ESEntry &entry = ESTables[i][indexEachWays[i]];
        if (!compareTags(entry.tag, tagEachWays[i])) {
            found = true;
            way = i;
            index = indexEachWays[i];
            entryCopy = entry;
            break;
        }
    }

    if (!found) {
        return {false, 0};
    }

    const int64_t stride = extendStride(entryCopy.stride);
    Addr predAddr = static_cast<Addr>(
            static_cast<int64_t>(entryCopy.lastAddr) +
            (inflights + 1) * stride);

    if (stride == 0) {
        return {false, predAddr};
    }

    if (isDcacheConfidenceLow(entryCopy)) {
        return {false, predAddr};
    }

    if (entryCopy.confidence < confidenceThreshold) {
        return {false, predAddr};
    }
    return {true, predAddr};
}

APResult
EStrideAP::addressPredict(APPredMetaData *predMetaData)
{
    assert(predMetaData);
    int inflights = inflightWindow.addToInflightWindow(predMetaData->pc);
    return doPredict(predMetaData, inflights);
}

void
EStrideAP::updateAddressPredictor(APUpdateMetaData *updateMetaData)
{
    assert(updateMetaData);

    inflightWindow.removeFromWindow(updateMetaData->pc, updateMetaData->seq_no);

    std::vector<uint32_t> indexEachWays(ways);
    for (size_t i = 0; i < indexEachWays.size(); i++) {
        indexEachWays[i] = pcHashToWayIndex(updateMetaData->pc, i);
    }

    std::vector<uint32_t> tagEachWays(ways);
    for (size_t i = 0; i < tagEachWays.size(); i++) {
        tagEachWays[i] = pcHashToTag(updateMetaData->pc, i);
    }

    bool found = false;
    int way = 0;
    uint32_t index = 0;
    for (size_t i = 0; i < ways; ++i) {
        const ESEntry &entry = ESTables[i][indexEachWays[i]];
        if (!compareTags(entry.tag, tagEachWays[i])) {
            found = true;
            way = i;
            index = indexEachWays[i];
            break;
        }
    }

    if (found) {
        ESEntry &entry = ESTables[way][index];

        if (updateMetaData->fromDcache) {
            entry.dcacheConfidence += 1;
        } else {
            entry.dcacheConfidence -= 3;
            return;
        }

        bool misprediction =
                !(updateMetaData->actualAddr == static_cast<Addr>(
                        static_cast<int64_t>(entry.lastAddr) +
                        extendStride(entry.stride)));
        int64_t actualStride = static_cast<int64_t>(updateMetaData->actualAddr)
                             - static_cast<int64_t>(entry.lastAddr);
        entry.lastAddr = updateMetaData->actualAddr;

        if (entry.notFirstAppear) {
            if (!misprediction) {
                UpdateConfDecision decision = decideToUpdate(actualStride);
                if (decision.first) {
                    entry.confidence =
                            std::min(MAXCONFIDENCE, entry.confidence + decision.second);
                }

                if (decision.first && entry.useful < 3) {
                    entry.useful++;
                }

                if (entry.confidence >= confidenceThreshold) {
                    entry.useful = 3;
                }
            } else {
                int confDec = (1 << ((logMaxConfidence + 1) / 2));
                if (entry.confidence - confDec > 0) {
                    entry.confidence -= confDec;
                } else {
                    entry.confidence = 0;
                    entry.useful = 0;
                }
                entry.notFirstAppear = 0;
            }
        } else {
            entry.stride = actualStride;
            entry.notFirstAppear++;
        }
        return;
    }

    if (!updateMetaData->fromDcache) {
        return;
    }

    uint32_t wayBegin = random_mt.random<uint32_t>() % ways;

    auto allocateEntry = [&](ESEntry &entry, uint32_t tag, Addr actualAddr) {
        entry.tag = tag;
        entry.confidence = 1;
        entry.stride = 0;
        entry.lastAddr = actualAddr;
        entry.useful = 0;
        entry.notFirstAppear = 0;
        entry.dcacheConfidence.saturate();
    };

    for (size_t i = 0; i < ways; ++i) {
        ESEntry &entry = ESTables[wayBegin][indexEachWays[wayBegin]];
        if (entry.confidence == 0) {
            allocateEntry(entry, tagEachWays[wayBegin], updateMetaData->actualAddr);
            return;
        }
        wayBegin = (wayBegin + 1) % ways;
    }

    for (size_t i = 0; i < ways; ++i) {
        ESEntry &entry = ESTables[wayBegin][indexEachWays[wayBegin]];
        if (extendStride(entry.stride) == 0) {
            allocateEntry(entry, tagEachWays[wayBegin], updateMetaData->actualAddr);
            return;
        }
        wayBegin = (wayBegin + 1) % ways;
    }

    for (size_t i = 0; i < ways; ++i) {
        ESEntry &entry = ESTables[wayBegin][indexEachWays[wayBegin]];
        if (isDcacheConfidenceLow(entry)) {
            allocateEntry(entry, tagEachWays[wayBegin], updateMetaData->actualAddr);
            return;
        }
        wayBegin = (wayBegin + 1) % ways;
    }

    for (size_t i = 0; i < ways; ++i) {
        ESEntry &entry = ESTables[wayBegin][indexEachWays[wayBegin]];
        if (entry.useful == 0) {
            allocateEntry(entry, tagEachWays[wayBegin], updateMetaData->actualAddr);
            return;
        }
        wayBegin = (wayBegin + 1) % ways;
    }

    ESEntry &entry = ESTables[wayBegin][indexEachWays[wayBegin]];
    if (entry.useful > 0 && tryDecUseful(entry) == 0) {
        entry.useful -= 1;
    }
}

void
EStrideAP::specUpdateAddressPredictor(APSpecUpdateMetaData *specUpdateMetaData)
{
    assert(specUpdateMetaData);
}

void
EStrideAP::squash(const uint64_t seq_no)
{
    inflightWindow.squash(seq_no);
}

} // namespace addresspred

} // namespace gem5
