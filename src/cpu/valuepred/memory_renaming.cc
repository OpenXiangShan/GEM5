#include "cpu/valuepred/memory_renaming.hh"

#include <algorithm>
#include <limits>

#include "base/random.hh"
#include "base/trace.hh"
#include "debug/MemoryRenaming.hh"

namespace gem5
{

namespace valuepred
{

MemoryRenaming::InflightWindow::InflightWindow(int windowTagLength,
                                               bool idealWindow)
    : Named("memoryRenamingInflightWindow"),
      windowTagLength(windowTagLength),
      idealWindow(idealWindow),
      windowActiveCount(0),
      lastSeqNo(0ul)
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
            return x & ((1 << targetLength) - 1);
        };
    }
}

int
MemoryRenaming::InflightWindow::addToInflightWindow(Addr pc)
{
    uint64_t key = hashMethod(pc, windowTagLength);

    ++windowActiveCount;
    DPRINTF(MemoryRenaming,
            "inflight window add[count: %d] => pc: %lX\n",
            windowActiveCount, pc);

    return windows[key]++;
}

void
MemoryRenaming::InflightWindow::removeFromWindow(Addr pc, uint64_t seq_no)
{
    if (seq_no <= lastSeqNo) {
        return;
    }

    --windowActiveCount;
    assert(windowActiveCount >= 0);

    uint64_t key = hashMethod(pc, windowTagLength);
    assert(windows.count(key));
    assert(windows[key] > 0);

    --windows[key];
    if (windows[key] == 0) {
        windows.erase(key);
    }

    DPRINTF(MemoryRenaming,
            "inflight window remove[count: %d] => pc: %lX\n",
            windowActiveCount, pc);
}

void
MemoryRenaming::InflightWindow::squash(uint64_t seq_no)
{
    DPRINTF(MemoryRenaming, "squash reset inflight window\n");
    windows.clear();
    windowActiveCount = 0;
    lastSeqNo = seq_no;
}

MemoryRenaming::MemoryRenaming(const Params &params)
    : VPUnit(params),
      ways(params.ways),
      tagWidth(params.tagWidth),
      logESTBEntrys(params.logESTBEntrys),
      entryCounts(1 << logESTBEntrys),
      logMaxConfidence(params.logMaxConfidence),
      MAXCONFIDENCE(1 << logMaxConfidence),
      confidenceThreshold(static_cast<int>(params.thresholdPercent * MAXCONFIDENCE)),
      logStoreLoadValueFileEntries(params.logStoreLoadValueFileEntries),
      nextValueFileIndex(0),
      inflightWindow(params.inflightWindowTagLength, params.idealWindow)
{
    gem5_assert(ways, "MemoryRenaming ways must > 0\n");
    gem5_assert(tagWidth, "MemoryRenaming tagWidth must > 0\n");
    gem5_assert(logESTBEntrys, "MemoryRenaming logESTBEntrys must > 0\n");
    gem5_assert(logMaxConfidence, "MemoryRenaming logMaxConfidence must > 0\n");
    gem5_assert(params.inflightWindowTagLength,
                "MemoryRenaming inflightWindowTagLength must > 0\n");

    storeLoadCacheTables.resize(ways);
    for (auto &table : storeLoadCacheTables) {
        table.resize(entryCounts);
    }

    DPRINTF(MemoryRenaming,
            "MemoryRenaming params: ways=%d tagWidth=%d entryCounts=%d "
            "maxConf=%d confThres=%d vfCfgLog2=%d vfEntries=INF\n",
            ways, tagWidth, entryCounts, MAXCONFIDENCE,
            confidenceThreshold, logStoreLoadValueFileEntries);
}

uint32_t
MemoryRenaming::pcHashToWayIndex(Addr pc, int way)
{
    uint64_t hash = pc;
    for (int k = 1; k <= ways; k++) {
        int shift = ((k * logESTBEntrys) - way) % 64;
        if (shift < 0) {
            shift += 64;
        }
        hash ^= (pc >> shift);
    }

    return hash & ((1u << logESTBEntrys) - 1);
}

uint32_t
MemoryRenaming::pcHashToTag(Addr pc, int way)
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

    return hash & ((1 << tagWidth) - 1);
}

uint32_t
MemoryRenaming::compareTags(uint32_t tag1, uint32_t tag2)
{
    return ((tag1 & ((1 << tagWidth) - 1)) ^ (tag2 & ((1 << tagWidth) - 1)));
}

VPResult
MemoryRenaming::doStoreLoadCacheLookup(VPPredMetaData *predMetaData)
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
    int way = -1;
    uint32_t index = 0;
    StoreLoadCacheEntry entryCopy;
    for (int i = 0; i < ways; ++i) {
        const StoreLoadCacheEntry &entry =
            storeLoadCacheTables[i][indexEachWays[i]];
        if (entry.valid && !compareTags(entry.tag, tagEachWays[i])) {
            found = true;
            way = i;
            index = indexEachWays[i];
            entryCopy = entry;
            break;
        }
    }

    if (!found) {
        DPRINTF(MemoryRenaming,
                "[SLC-Predict]=> seq_no:%lu load_pc:%lX miss\n",
                predMetaData->seq_no, predMetaData->pc);
        return {false, 0};
    }

    DPRINTF(MemoryRenaming,
            "seq_no:%lu load_pc:%lX hit [way:%d index:%u vfi:%llu conf:%d useful:%d hasProducer:%s producerPC:%lX]\n",
            predMetaData->seq_no, predMetaData->pc, way, index,
            static_cast<unsigned long long>(entryCopy.valueFileIndex), entryCopy.confidence, entryCopy.useful,
            entryCopy.hasProducerStorePC ? "yes" : "no",
            entryCopy.producerStorePC);

    // Stage-1 memory renaming only provides SLC lookup information.
    // Value-file based speculative value injection is added in a later step.
    return {false, static_cast<RegVal>(entryCopy.valueFileIndex)};
}

VPResult
MemoryRenaming::valuePredict(VPPredMetaData *predMetaData)
{
    gem5_assert(predMetaData, "can't pass nullptr to vpunit\n");

    inflightWindow.addToInflightWindow(predMetaData->pc);
    return doStoreLoadCacheLookup(predMetaData);
}

void
MemoryRenaming::updateStoreLoadCache(VPUpdateMetaData *updateMetaData)
{
    std::vector<uint32_t> indexEachWays(ways);
    for (size_t i = 0; i < indexEachWays.size(); i++) {
        indexEachWays[i] = pcHashToWayIndex(updateMetaData->pc, i);
    }

    std::vector<uint32_t> tagEachWays(ways);
    for (size_t i = 0; i < tagEachWays.size(); i++) {
        tagEachWays[i] = pcHashToTag(updateMetaData->pc, i);
    }

    bool found = false;
    int way = -1;
    uint32_t index = 0;
    for (size_t i = 0; i < ways; ++i) {
        const StoreLoadCacheEntry &entry =
            storeLoadCacheTables[i][indexEachWays[i]];
        if (entry.valid && !compareTags(entry.tag, tagEachWays[i])) {
            found = true;
            way = i;
            index = indexEachWays[i];
            break;
        }
    }

    if (found) {
        StoreLoadCacheEntry &entry = storeLoadCacheTables[way][index];
        if (updateMetaData->hasProducerStorePC) {
            if (entry.hasProducerStorePC &&
                entry.producerStorePC == updateMetaData->producerStorePC) {
                entry.confidence = std::min(MAXCONFIDENCE, entry.confidence + 1);
                if (entry.useful < 3) {
                    entry.useful++;
                }
            } else {
                const int confDec = (1 << ((logMaxConfidence + 1) / 2));
                if (entry.confidence > confDec) {
                    entry.confidence -= confDec;
                } else {
                    entry.confidence = 0;
                    entry.useful = 0;
                }
                entry.hasProducerStorePC = true;
                entry.producerStorePC = updateMetaData->producerStorePC;
                if (entry.confidence == 0) {
                    entry.confidence = 1;
                }
            }

            if (entry.confidence >= confidenceThreshold) {
                entry.useful = 3;
            }
        }

        DPRINTF(MemoryRenaming,
                "load_pc:%lX hit [way:%d index:%u vfi:%llu conf:%d useful:%d hasProducer:%s producerPC:%lX]\n",
                updateMetaData->pc, way, index,
                static_cast<unsigned long long>(entry.valueFileIndex),
                entry.confidence, entry.useful,
                entry.hasProducerStorePC ? "yes" : "no",
                entry.producerStorePC);
        return;
    }

    uint32_t wayBegin = random_mt.random<uint32_t>() % ways;
    int allocWay = -1;

    for (size_t i = 0; i < ways; ++i) {
        uint32_t wayTry = (wayBegin + i) % ways;
        StoreLoadCacheEntry &entry =
            storeLoadCacheTables[wayTry][indexEachWays[wayTry]];
        if (!entry.valid) {
            allocWay = wayTry;
            break;
        }
    }

    if (allocWay < 0) {
        for (size_t i = 0; i < ways; ++i) {
            uint32_t wayTry = (wayBegin + i) % ways;
            StoreLoadCacheEntry &entry =
                storeLoadCacheTables[wayTry][indexEachWays[wayTry]];
            if (entry.confidence == 0 || entry.useful == 0) {
                allocWay = wayTry;
                break;
            }
        }
    }

    if (allocWay < 0) {
        StoreLoadCacheEntry &entry =
            storeLoadCacheTables[wayBegin][indexEachWays[wayBegin]];
        if (entry.useful > 0 && ((random_mt.random<uint32_t>() & 0x3) == 0)) {
            entry.useful--;
        }
        DPRINTF(MemoryRenaming,
                "[SLC-Update] load_pc:%lX no replacement candidate [way:%u index:%u], try decay useful\n",
                updateMetaData->pc, wayBegin, indexEachWays[wayBegin]);
        return;
    }

    StoreLoadCacheEntry &entry =
        storeLoadCacheTables[allocWay][indexEachWays[allocWay]];
    entry.valid = true;
    entry.tag = tagEachWays[allocWay];
    entry.valueFileIndex = nextValueFileIndex;
    if (nextValueFileIndex != std::numeric_limits<uint64_t>::max()) {
        ++nextValueFileIndex;
    }
    entry.confidence = updateMetaData->hasProducerStorePC ? 1 : 0;
    entry.useful = 0;
    entry.hasProducerStorePC = updateMetaData->hasProducerStorePC;
    entry.producerStorePC = updateMetaData->hasProducerStorePC ?
                            updateMetaData->producerStorePC : 0;

    DPRINTF(MemoryRenaming,
            "[SLC-Update] load_pc:%lX allocate [way:%d index:%u vfi:%llu hasProducer:%s producerPC:%lX]\n",
            updateMetaData->pc, allocWay, indexEachWays[allocWay],
            static_cast<unsigned long long>(entry.valueFileIndex),
            entry.hasProducerStorePC ? "yes" : "no",
            entry.producerStorePC);
}

void
MemoryRenaming::updateValuePredictor(VPUpdateMetaData *updateMetaData)
{
    gem5_assert(updateMetaData, "can't pass nullptr to vpunit\n");

    inflightWindow.removeFromWindow(updateMetaData->pc, updateMetaData->seq_no);
    updateStoreLoadCache(updateMetaData);
}

void
MemoryRenaming::specUpdateValuePredictor(VPSpecUpdateMetaData *specUpdateMetaData)
{
    gem5_assert(specUpdateMetaData, "can't pass nullptr to vpunit\n");
}

void
MemoryRenaming::squash(const uint64_t seq_no)
{
    inflightWindow.squash(seq_no);
}

} // namespace valuepred

} // namespace gem5
