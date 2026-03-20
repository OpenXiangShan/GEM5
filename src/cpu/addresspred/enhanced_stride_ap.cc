#include "cpu/addresspred/enhanced_stride_ap.hh"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <sstream>

#include "base/logging.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "debug/APCOMMON.hh"

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

bool
isOlderThanSquashVersion(uint8_t instVersion, uint8_t squashVersion)
{
    constexpr uint8_t kVersionLimit = 16;
    constexpr uint8_t kMaxInflightSquash = 7;

    const bool larger = squashVersion > instVersion &&
                        squashVersion - instVersion <= kMaxInflightSquash;
    const bool wrapped_larger =
            squashVersion + kVersionLimit > instVersion &&
            squashVersion + kVersionLimit - instVersion <= kMaxInflightSquash;
    return larger || wrapped_larger;
}

} // namespace

EStrideAP::InflightWindow::InflightWindow(int windowTagLength, bool idealWindow,
                                          bool idealInflightWindow)
    : windowTagLength(windowTagLength),
      idealInflightWindow(idealInflightWindow)
{
    if (idealInflightWindow) {
        return;
    }

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

size_t
EStrideAP::InflightWindow::idealWindowEntryCount() const
{
    size_t total = 0;
    for (const auto &[pc, seqNums] : idealWindows) {
        total += seqNums.size();
    }
    return total;
}

std::string
EStrideAP::InflightWindow::formatSeqNumsForDebug(
        const std::set<uint64_t> &seqNums) const
{
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (uint64_t seqNum : seqNums) {
        if (!first) {
            oss << ", ";
        }
        first = false;
        oss << seqNum;
    }
    oss << "]";
    return oss.str();
}

std::string
EStrideAP::InflightWindow::formatIdealWindowForDebug() const
{
    std::ostringstream oss;
    oss << "{pc_count=" << idealWindows.size()
        << ", seq_count=" << idealWindowEntryCount()
        << ", windows=[";

    bool firstPc = true;
    for (const auto &[pc, seqNums] : idealWindows) {
        if (!firstPc) {
            oss << "; ";
        }
        firstPc = false;
        oss << "pc=0x" << std::hex << pc << std::dec
            << " seqs=" << formatSeqNumsForDebug(seqNums);
    }
    oss << "]}";
    return oss.str();
}

int
EStrideAP::InflightWindow::addToInflightWindow(Addr pc, uint64_t seq_no)
{
    if (idealInflightWindow) {
        auto &seqNums = idealWindows[pc];
        const std::string seqNumsBefore = formatSeqNumsForDebug(seqNums);
        auto insertResult = seqNums.insert(seq_no);
        gem5_assert(insertResult.second,
                    "Ideal inflight window sees duplicated seq_no for pc\n");
        const int inflights = static_cast<int>(seqNums.size()) - 1;
        DPRINTF(APCOMMON,
                "Ideal inflight add pc=0x%lx seq=%llu before=%s after=%s "
                "inflights=%d\n",
                pc, seq_no, seqNumsBefore.c_str(),
                formatSeqNumsForDebug(seqNums).c_str(), inflights);
        return inflights;
    }

    uint64_t key = hashMethod(pc, windowTagLength);
    int inflights = windows[key]++;
    DPRINTF(APCOMMON,
            "Legacy inflight add pc=0x%lx seq=%llu key=0x%lx inflights=%d "
            "new_count=%d\n",
            pc, seq_no, key, inflights, windows[key]);
    return inflights;
}

void
EStrideAP::InflightWindow::removeFromWindow(Addr pc, uint64_t seq_no)
{
    if (idealInflightWindow) {
        DPRINTF(APCOMMON,
                "Ideal inflight remove begin pc=0x%lx seq=%llu state=%s\n",
                pc, seq_no, formatIdealWindowForDebug().c_str());
        auto mapIt = idealWindows.find(pc);
        gem5_assert(mapIt != idealWindows.end(),
                    "Ideal inflight window misses pc on commit\n");

        auto &seqNums = mapIt->second;
        DPRINTF(APCOMMON,
                "Ideal inflight remove target before pc=0x%lx seqs=%s\n",
                pc, formatSeqNumsForDebug(seqNums).c_str());
        auto seqIt = seqNums.find(seq_no);
        gem5_assert(seqIt != seqNums.end(),
                    "Ideal inflight window misses seq_no on commit\n");
        seqNums.erase(seqIt);
        const bool erasePc = seqNums.empty();
        const std::string seqNumsAfter = formatSeqNumsForDebug(seqNums);
        if (seqNums.empty()) {
            idealWindows.erase(mapIt);
        }
        DPRINTF(APCOMMON,
                "Ideal inflight remove end pc=0x%lx seq=%llu erased_pc=%d "
                "pc_after=%s state=%s\n",
                pc, seq_no, erasePc, erasePc ? "[]" : seqNumsAfter.c_str(),
                formatIdealWindowForDebug().c_str());
        return;
    }

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
    lastSeqNo = seq_no;
    if (idealInflightWindow) {
        DPRINTF(APCOMMON,
                "Ideal inflight squash begin squash_seq=%llu state=%s\n",
                seq_no, formatIdealWindowForDebug().c_str());
        for (auto it = idealWindows.begin(); it != idealWindows.end();) {
            const Addr pc = it->first;
            auto &seqNums = it->second;
            const std::string seqNumsBefore = formatSeqNumsForDebug(seqNums);
            std::set<uint64_t> removedSeqNums(
                    seqNums.upper_bound(seq_no), seqNums.end());
            seqNums.erase(seqNums.upper_bound(seq_no), seqNums.end());
            if (!removedSeqNums.empty()) {
                DPRINTF(APCOMMON,
                        "Ideal inflight squash pc=0x%lx removed=%s before=%s "
                        "after=%s\n",
                        pc,
                        formatSeqNumsForDebug(removedSeqNums).c_str(),
                        seqNumsBefore.c_str(),
                        formatSeqNumsForDebug(seqNums).c_str());
            }
            if (seqNums.empty()) {
                it = idealWindows.erase(it);
            } else {
                ++it;
            }
        }
        DPRINTF(APCOMMON,
                "Ideal inflight squash end squash_seq=%llu state=%s\n",
                seq_no, formatIdealWindowForDebug().c_str());
        return;
    }

    windows.clear();
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
      decodeToFetchDelay(params.decodeToFetchDelay),
      delayedSquashEvent(
              [this] { processDelayedSquash(); },
              "EStrideAP delayed squash",
              false,
              Event::Priority(Event::CPU_Tick_Pri + 1)),
      inflightWindow(
              params.inflightWindowTagLength,
              params.idealWindow,
              params.idealInflightWindow)
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
    gem5_assert(params.idealInflightWindow || params.inflightWindowTagLength > 0,
                "EStrideAP inflightWindowTagLength must > 0\n");

    ESTables.resize(ways);
    for (auto &table : ESTables) {
        table.assign(entryCounts, ESEntry(dcacheCounterBits));
    }
}

void
EStrideAP::scheduleDelayedSquash(uint64_t seq_no)
{
    const Tick apply_tick = clockEdge(decodeToFetchDelay);
    delayedSquashQueue.push_back({apply_tick, seq_no});

    DPRINTF(APCOMMON,
            "AP enqueue delayed squash seq=%llu apply_tick=%llu delay_cycles=%llu\n",
            seq_no, static_cast<unsigned long long>(apply_tick),
            static_cast<unsigned long long>(decodeToFetchDelay));

    if (!delayedSquashEvent.scheduled()) {
        schedule(delayedSquashEvent, apply_tick);
    }
}

void
EStrideAP::processDelayedSquash()
{
    while (!delayedSquashQueue.empty() &&
           delayedSquashQueue.front().applyTick <= curTick()) {
        auto req = delayedSquashQueue.front();
        delayedSquashQueue.pop_front();

        // if (req.seqNo <= inflightWindow.lastSeqNo) {
        //     DPRINTF(APCOMMON,
        //             "AP skip stale delayed squash seq=%llu last_squash_seq=%llu\n",
        //             req.seqNo, inflightWindow.lastSeqNo);
        //     continue;
        // }

        DPRINTF(APCOMMON,
                "AP apply delayed squash seq=%llu at tick=%llu\n",
                req.seqNo, static_cast<unsigned long long>(curTick()));
        inflightWindow.squash(req.seqNo);
    }

    if (!delayedSquashQueue.empty()) {
        schedule(delayedSquashEvent, delayedSquashQueue.front().applyTick);
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
        DPRINTF(APCOMMON,
                "AP predict miss [sn:%llu] pc=0x%lx inflights=%d\n",
                predMetaData->seq_no, predMetaData->pc, inflights);
        return {false, 0};
    }

    const int64_t stride = extendStride(entryCopy.stride);
    Addr predAddr = static_cast<Addr>(
            static_cast<int64_t>(entryCopy.lastAddr) +
            (inflights + 1) * stride);

    DPRINTF(APCOMMON,
            "AP predict calc [sn:%llu] pc=0x%lx inflights=%d "
            "lastAddr=0x%lx rawStride=%lld extStride=%lld predAddr=0x%lx "
            "conf=%d/%d dcacheSat=%.6f threshold=%d\n",
            predMetaData->seq_no, predMetaData->pc, inflights,
            entryCopy.lastAddr, entryCopy.stride, stride, predAddr,
            entryCopy.confidence, MAXCONFIDENCE,
            entryCopy.dcacheConfidence.calcSaturation(), confidenceThreshold);

    if (stride == 0) {
        DPRINTF(APCOMMON,
                "AP predict reject [sn:%llu] pc=0x%lx reason=stride_zero "
                "predAddr=0x%lx\n",
                predMetaData->seq_no, predMetaData->pc, predAddr);
        return {false, predAddr};
    }

    if (isDcacheConfidenceLow(entryCopy)) {
        DPRINTF(APCOMMON,
                "AP predict reject [sn:%llu] pc=0x%lx reason=dcache_conf_low "
                "dcacheSat=%.6f threshold=%.6f predAddr=0x%lx\n",
                predMetaData->seq_no, predMetaData->pc,
                entryCopy.dcacheConfidence.calcSaturation(),
                dcacheThresholdPercent, predAddr);
        return {false, predAddr};
    }

    if (entryCopy.confidence < confidenceThreshold) {
        DPRINTF(APCOMMON,
                "AP predict reject [sn:%llu] pc=0x%lx reason=confidence_low "
                "conf=%d threshold=%d predAddr=0x%lx\n",
                predMetaData->seq_no, predMetaData->pc,
                entryCopy.confidence, confidenceThreshold, predAddr);
        return {false, predAddr};
    }
    DPRINTF(APCOMMON,
            "AP predict take [sn:%llu] pc=0x%lx predAddr=0x%lx inflights=%d\n",
            predMetaData->seq_no, predMetaData->pc, predAddr, inflights);
    return {true, predAddr};
}

APResult
EStrideAP::addressPredict(APPredMetaData *predMetaData)
{
    assert(predMetaData);
    if (isOlderThanSquashVersion(predMetaData->inst_version, lastSquashVersion)) {
        DPRINTF(APCOMMON,
                "AP predict skip [sn:%llu] pc=0x%lx reason=%s "
                "instVer=%u lastSquashVer=%u reqSquashVer=%u\n",
                predMetaData->seq_no, predMetaData->pc, "stale squash version",
                predMetaData->inst_version, lastSquashVersion,
                predMetaData->squash_version);
        return {false, 0};
    }

    int inflights = inflightWindow.addToInflightWindow(
            predMetaData->pc, predMetaData->seq_no);
    return doPredict(predMetaData, inflights);
}

void
EStrideAP::updateAddressPredictor(APUpdateMetaData *updateMetaData)
{
    assert(updateMetaData);

    if (updateMetaData->apPredictCalled) {
        inflightWindow.removeFromWindow(updateMetaData->pc, updateMetaData->seq_no);
    }

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
                        extendStride(entry.stride))) || updateMetaData->isMisprediction;
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
                // int confDec = (1 << ((logMaxConfidence + 1) / 2));
                int confDec = (1 << logMaxConfidence) / 2;
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
    DPRINTF(APCOMMON, "AP delayed squash seq=%llu keep squashVer=%u\n",
            seq_no, lastSquashVersion);
    scheduleDelayedSquash(seq_no);
}

void
EStrideAP::squash(const uint64_t seq_no, uint8_t squash_version)
{
    lastSquashVersion = squash_version;
    DPRINTF(APCOMMON, "AP squash update seq=%llu squashVer=%u\n",
            seq_no, squash_version);
    inflightWindow.squash(seq_no);
}

} // namespace addresspred

} // namespace gem5
