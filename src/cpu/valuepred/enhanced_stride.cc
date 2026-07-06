/*
 * EStride is largely based on the open-source implementation of the
 * 1st-place Championship Value Prediction (CVP-1) submission.
 *
 * The version in this codebase is adapted and modified for our environment
 * by Yibo Zhang <yb_zhang@mail.ustc.edu.cn>; it is not intended to be a
 * bit-exact copy of the original code.
 *
 * For detailed background and reference material:
 * Paper: https://microarch.org/cvp1/papers/Seznec.pdf
 * Open-source implementation: https://www.microarch.org/cvp1/code/Seznec.tar.gz
 * Official website: https://www.microarch.org/cvp1/
 */

#include "cpu/valuepred/enhanced_stride.hh"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "base/output.hh"
#include "base/random.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/valuepred/es_metadata.hh"
#include "debug/EStride.hh"

namespace gem5
{

namespace valuepred
{
//*********************** InflightWindow *************************
EStride::InflightWindow::InflightWindow(int windowTagLength, bool idealWindow)
    : Named("inflightWindow"),
      windowTagLength(windowTagLength),
      idealWindow(idealWindow),
      windowActiveCount(0),
      lastSeqNo(0ul)
{
    // simple hash plan
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
EStride::InflightWindow::addToInflightWindow(Addr pc)
{

    uint64_t key = hashMethod(pc, windowTagLength);

    ++windowActiveCount;
    DPRINTF(EStride, "inflight window add[count: %d]  => pc: %lX \n", windowActiveCount, pc);

    return windows[key]++;
}

void
EStride::InflightWindow::removeFromWindow(Addr pc, uint64_t seq_no)
{
    if (seq_no <= lastSeqNo) {
        return;
    }

    --windowActiveCount;
    assert(windowActiveCount >= 0);
    DPRINTF(EStride, "inflight window remove[count: %d]  => pc: %lX \n", windowActiveCount, pc);

    uint64_t key = hashMethod(pc, windowTagLength);
    assert(windows.count(key));
    assert(windows[key] > 0);

    int inflightCount = windows[key];
    --windows[key];
    if (windows[key] == 0) {
        windows.erase(key);
    }
}

void
EStride::InflightWindow::squash(uint64_t seq_no)
{
    DPRINTF(EStride, "squash make inflight count window be zero\n");
    windows.clear();
    windowActiveCount = 0;
    lastSeqNo = seq_no;
}

//****************************************************************


EStride::EStride(const Params &params)
    : VPUnit(params),
      ways(params.ways),
      strideWidth(params.strideWidth),
      tagWidth(params.tagWidth),
      logESTBEntrys(params.logESTBEntrys),
      entryCounts(1 << logESTBEntrys),
      logMaxConfidence(params.logMaxConfidence),
      MAXCONFIDENCE(1 << logMaxConfidence),
      confidenceThreshold(static_cast<int>(params.thresholdPercent * MAXCONFIDENCE)),
      inflightWindows(),
      enableTimeMsgInUpdate(params.enableTimeMsgInUpdate),
      esstats(this)
{
    // assertion
    gem5_assert(ways, "EStride ways must > 0 \n");
    gem5_assert(strideWidth, "EStride strideWidth must > 0 \n");
    gem5_assert(strideWidth < 64, "EStride strideWidth must < 64 \n");
    gem5_assert(tagWidth, "EStride tagWidth must > 0 \n");
    gem5_assert(logESTBEntrys, "EStride logESTBEntrys must > 0 \n");
    gem5_assert(logMaxConfidence, "EStride logMaxConfidence must > 0 \n");
    gem5_assert(params.inflightWindowTagLength, "EStride inflightWindowTagLength must > 0 \n");

    // init stats
    inflightWindows.reserve(numThreads);
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        inflightWindows.emplace_back(params.inflightWindowTagLength, params.idealWindow);
    }

    ESTables.resize(numThreads);
    for (auto &threadTables : ESTables) {
        threadTables.resize(ways);
        for (auto &table : threadTables) {
            table.resize(entryCounts);
        }
    }

    esstats.allocate.init(ways, entryCounts);
    esstats.strideNotEquals.init(ways, entryCounts);
    esstats.strideEquals.init(ways, entryCounts);
    // The global inflight window size should be slightly larger than the ROB.
    esstats.inflightSH.init(400);

    DPRINTF(EStride, "================================== EStride Params ==================================\n");
    DPRINTF(EStride, "ways: %7d | strideWidth: %7d bits | tagWidth %7d bits\n", ways, strideWidth, tagWidth);
    DPRINTF(EStride, "EStride table entrys: %7d\n", entryCounts);
    DPRINTF(EStride, "MAXCONFIDENCE: %7d | confidenceThreshold %7d\n", MAXCONFIDENCE, confidenceThreshold);
    DPRINTF(EStride, "inflightWindowTagLength: %7d \n", params.inflightWindowTagLength);
    DPRINTF(EStride, "enableTimeMsgInUpdate(need pmu support): %s\n", enableTimeMsgInUpdate ? "yes" : "no");
    DPRINTF(EStride, "==================================  end of Params ==================================\n");

}

//*********************** auxilary method **************************

int64_t
EStride::extendStride(int64_t entryStride)
{
    int64_t mask = (1 << strideWidth) - 1;
    entryStride &= mask;

    int64_t sign_bit = 1 << (strideWidth - 1);

    if (entryStride & sign_bit) {
        entryStride |= (~mask);
    } else {
        entryStride &= mask;
    }

    return entryStride;
}

uint32_t
EStride::pcHashToWayIndex(Addr pc, int way)
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
EStride::pcHashToTag(Addr pc, int way)
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
EStride::compareTags(uint32_t tag1, uint32_t tag2)
{
    return ((tag1 & ((1 << tagWidth) - 1)) ^ (tag2 & ((1 << tagWidth) - 1)));
}


EStride::UpdateConfDecision
EStride::decideToUpdate(bool isLoadInst, uint64_t inflightTime, int64_t stride)
{
    // todo: different update strategy
    // todo: The current update strategy is close to 100% probability and will
    // have to be changed subsequently.
    auto tryUpdateOnce = [&]() -> bool {
        // uint64_t inflightTimeLocal = inflightTime;
        // int quick = inflightTime <= FASTINSTTIME;
        // int notl1miss = inflightTime <= L1HITMAXTIME;
        // int notl2miss = inflightTime <= L2HITMAXTIME;
        // int notl3miss = inflightTime <= L3HITMAXTIME;
        // int isLoad = isLoadInst;

        // int denominator = (1 << (notl1miss + notl2miss + notl3miss + 2 * quick + 2 * !isLoad)) - 1;

        // return (!esUpdateMetaData->actualValue || stride) && ((random_mt.random<int>() & denominator) == 0);


        // try once func should support more func to DSE
        return true;
    };


    bool shouldUpdate = tryUpdateOnce();
    // Longer strides will get more update possibilities.
    if (std::abs(stride) >= 8) {
        shouldUpdate |= tryUpdateOnce();
    }
    if (std::abs(stride) >= 64) {
        shouldUpdate |= tryUpdateOnce();
    }

    bool finalUpdate = shouldUpdate & ((std::abs(stride) > 1) || !isLoadInst ||
                                       ((stride == -1) & ((random_mt.random<int32_t>() & 1) == 0)) ||
                                       ((stride == 1) & ((random_mt.random<int32_t>() & 3) == 0)));

    return {finalUpdate, 1};
}

uint32_t
EStride::tryDecUseful(const ESEntry &entry)
{
    uint32_t k = 2 + 2 * (entry.confidence > MAXCONFIDENCE / 8) + 2 * (entry.confidence >= MAXCONFIDENCE / 4);
    uint32_t mask = (1 << k) - 1;

    return random_mt.random<uint32_t>() & mask;
}
//****************************************************************

VPResult
EStride::doPredict(Addr pc, uint64_t seqNo, ThreadID tid, int inflights)
{
    std::vector<uint32_t> indexEachWays(ways);
    for (size_t i = 0; i < indexEachWays.size(); i++) {
        indexEachWays[i] = pcHashToWayIndex(pc, i);
    }

    std::vector<uint32_t> tagEachWays(ways);
    for (size_t i = 0; i < tagEachWays.size(); i++) {
        tagEachWays[i] = pcHashToTag(pc, i);
    }

    if (debug::EStride) {
        std::string indexMarker, tagMarker;
        for (size_t i = 0; i < indexEachWays.size(); i++) {
            indexMarker += std::to_string(indexEachWays[i]);
            indexMarker += " ";
        }

        for (size_t i = 0; i < tagEachWays.size(); i++) {
            tagMarker += std::to_string(tagEachWays[i]);
            tagMarker += " ";
        }

        DPRINTF(EStride, "[ESPredict] seq_no:%lu pc: %lX index each way: %s\n", seqNo,
                pc, indexMarker.c_str());
        DPRINTF(EStride, "[ESPredict] seq_no:%lu pc: %lX tag each way: %s\n", seqNo,
                pc, tagMarker.c_str());
    }

    bool found = false;
    int way;
    uint32_t index;
    ESEntry entryCopy;
    for (int i = 0; i < ways; ++i) {
        const ESEntry &entry = ESTables[tid][i][indexEachWays[i]];
        if (!compareTags(entry.tag, tagEachWays[i])) {
            found = true;
            way = i;
            index = indexEachWays[i];
            entryCopy = entry;
            break;
        }
    }


    if (!found) {
        DPRINTF(EStride, "[ESPredict]=> seq_no:%lu pc:%lX not found in ES\n", seqNo,
                pc);
        return {false, 0};
    }

    DPRINTF(EStride, "[ESPredict][way: %d index: %u][confidence: %d  useful: %d lastValue: %lu]\n", way, index,
            entryCopy.confidence, entryCopy.useful, entryCopy.lastValue);

    uint64_t predValue = (uint64_t)((int64_t)entryCopy.lastValue + (inflights + 1) * extendStride(entryCopy.stride));

    if (entryCopy.confidence < confidenceThreshold) {
        DPRINTF(EStride,
                "[ESPredict]=> seq_no:%lu pc:%lX confidence not enough in ES[way: %d index: %u inflights: %d]\n",
                seqNo, pc, way, index, inflights);
        return {false, predValue};
    }

    DPRINTF(EStride, "[ESPredict]=> seq_no:%lu pc:%lX get prediction in [way: %d index: %u inflights: %d]\n",
            seqNo, pc, way, index, inflights);
    return {true, predValue};
}

VPPredictionCandidate
EStride::predict(const VPPredictRequest &request)
{
    assertValidTid(request.tid);
    int inflights = inflightWindows[request.tid].addToInflightWindow(request.pc);
    esstats.inflightSH.sample(inflights, 1);
    VPPredictionCandidate candidate;
    candidate.result = doPredict(request.pc, request.seqNo, request.tid,
                                 inflights);
    if (candidate.result.speculative) {
        candidate.record = std::make_unique<VPPredictionRecord>();
        candidate.record->offeredPrediction = true;
        candidate.record->predictedValue = candidate.result.value;
    }
    return candidate;
}

void
EStride::doUpdate(const UpdateContext &updateContext)
{
    assertValidTid(updateContext.tid);
    const ThreadID tid = updateContext.tid;


    // the first step update inflights window
    inflightWindows[tid].removeFromWindow(updateContext.pc, updateContext.seqNo);

    // Given the nature of the current hash method, the same PC gets the
    // same hash value every time it is computed. So instead of storing
    // the hash value in dyninst, we now compute it each time it is used.
    std::vector<unsigned> indexEachWays(ways);
    for (size_t i = 0; i < indexEachWays.size(); i++) {
        indexEachWays[i] = pcHashToWayIndex(updateContext.pc, i);
    }

    std::vector<unsigned> tagEachWays(ways);
    for (size_t i = 0; i < tagEachWays.size(); i++) {
        tagEachWays[i] = pcHashToTag(updateContext.pc, i);
    }


    if (debug::EStride) {
        std::string indexMarker, tagMarker;

        for (size_t i = 0; i < indexEachWays.size(); i++) {
            indexMarker += std::to_string(indexEachWays[i]);
            indexMarker += " ";
        }

        for (size_t i = 0; i < tagEachWays.size(); i++) {
            tagMarker += std::to_string(tagEachWays[i]);
            tagMarker += " ";
        }

        DPRINTF(EStride, "[ESUpdate] seq_no:%lu pc: %lX index each way: %s\n", updateContext.seqNo,
                updateContext.pc, indexMarker.c_str());
        DPRINTF(EStride, "[ESUpdate] seq_no:%lu pc: %lX tag each way: %s\n", updateContext.seqNo,
                updateContext.pc, tagMarker.c_str());
    }


    // try to find
    bool found = false;
    int way;
    uint32_t index;
    for (size_t i = 0; i < ways; ++i) {
        const ESEntry &entry = ESTables[tid][i][indexEachWays[i]];
        // todo maybe change the occupied
        if (!compareTags(entry.tag, tagEachWays[i])) {
            found = true;
            way = i;
            index = indexEachWays[i];
            break;
        }
    }


    if (found) {
        // update
        ESEntry &entry = ESTables[tid][way][index];
        DPRINTF(EStride, "[way: %d index: %u][confidence: %d  useful: %d lastValue: %lu]\n", way, index,
                entry.confidence, entry.useful, entry.lastValue);

        // update stride and judge is mispredict, and update lastValue
        DPRINTF(EStride, "activate value - last value: %lu-%lu, appear before: %s \n", updateContext.actualValue,
                entry.lastValue, entry.NotFirstAppear ? "not" : "yes");
        bool misprediction =
            !(updateContext.actualValue == (uint64_t)((int64_t)entry.lastValue + extendStride(entry.stride)));
        int64_t actualStride = updateContext.actualValue - entry.lastValue;
        entry.lastValue = updateContext.actualValue;

        if (entry.NotFirstAppear) {
            DPRINTF(EStride, "activate stride - entry stride = %d - %d \n", actualStride, entry.stride);
            if (!misprediction) {
                DPRINTF(EStride, "right stride\n");
                esstats.strideEquals[way][index]++;

                // mark down some instruction message
                if (!fluentInstructions.count(updateContext.pc)) {
                    fluentInstructions[updateContext.pc] = {1, std::string(updateContext.disas)};
                } else {
                    fluentInstructions[updateContext.pc].first += 1;
                }

                UpdateConfDecision decision =
                    decideToUpdate(updateContext.isLoadInst,
                                   updateContext.inflightTime,
                                   actualStride);
                if (decision.first) {
                    entry.confidence = std::min(MAXCONFIDENCE, entry.confidence + decision.second);
                }

                if (decision.first && entry.useful < 3) {
                    entry.useful++;
                }

                if (entry.confidence >= confidenceThreshold) {
                    entry.useful = 3;
                }
            } else {
                // misprediction
                // update confidence and useful

                DPRINTF(EStride, "error stride\n");
                esstats.strideNotEquals[way][index]++;
                int confDec = (1 << ((logMaxConfidence + 1) / 2));
                if (entry.confidence - confDec > 0) {
                    entry.confidence -= confDec;
                } else {
                    entry.confidence = 0;
                    entry.useful = 0;
                }
                entry.NotFirstAppear = 0;
            }
        } else {
            DPRINTF(EStride, "[ESFirstStride]before stride: %d => after stride: %d\n", entry.stride, actualStride);
            entry.stride = actualStride;
            entry.NotFirstAppear++;
        }

    } else {
        DPRINTF(EStride, "try to allocate new entry\n");

        // random find begin of the way-number to find allocate entry
        uint32_t wayBegin = random_mt.random<uint32_t>() % ways;

        // the entry have no stride at first allocate time

        // first find no confidence
        for (size_t i = 0; i < ways; ++i) {
            ESEntry &entry = ESTables[tid][wayBegin][indexEachWays[wayBegin]];
            if (entry.confidence == 0) {
                DPRINTF(EStride, "allocate by confidence: [way: %d index: %u] \n", wayBegin, indexEachWays[wayBegin]);
                esstats.allocate[wayBegin][indexEachWays[wayBegin]]++;
                entry.tag = tagEachWays[wayBegin];
                entry.confidence = 1;
                entry.stride = 0;
                entry.lastValue = updateContext.actualValue;
                entry.useful = 0;
                entry.NotFirstAppear = 0;
                return;
            }
            wayBegin = (wayBegin + 1) % ways;
        }

        // second find not useful
        for (size_t i = 0; i < ways; ++i) {
            ESEntry &entry = ESTables[tid][wayBegin][indexEachWays[wayBegin]];
            if (entry.useful == 0) {
                DPRINTF(EStride, "allocate by useful: [way: %d index: %u] \n", wayBegin, indexEachWays[wayBegin]);
                esstats.allocate[wayBegin][indexEachWays[wayBegin]]++;
                entry.tag = tagEachWays[wayBegin];
                entry.confidence = 1;
                entry.stride = 0;
                entry.lastValue = updateContext.actualValue;
                entry.useful = 0;
                entry.NotFirstAppear = 0;
                return;
            }
            wayBegin = (wayBegin + 1) % ways;
        }

        // can't allocate, just random dec some useful count
        ESEntry &entry = ESTables[tid][wayBegin][indexEachWays[wayBegin]];
        DPRINTF(EStride, "try dec useful \n");
        if (tryDecUseful(entry) == 0) {
            DPRINTF(EStride, "[dec useful count]=> way: %d index: %d", wayBegin, indexEachWays[wayBegin]);
            entry.useful -= 1;
        }
    }
}

void
EStride::update(const VPUpdateInfo &updateInfo, const VPPredictionRecord *record,
        const VPFeedback &feedback)
{
    (void)record;
    (void)feedback;
    UpdateContext updateContext;
    updateContext.pc = updateInfo.pc;
    updateContext.seqNo = updateInfo.seqNo;
    updateContext.tid = updateInfo.tid;
    updateContext.actualValue = updateInfo.actualValue;
    if (auto *esExt = updateInfo.getExt<ESUpdateInfoExt>()) {
        updateContext.isLoadInst = esExt->isLoadInst;
        updateContext.inflightTime = esExt->inflightTime;
        updateContext.disas = esExt->disas;
    }
    doUpdate(updateContext);
}

void
EStride::specUpdate(const VPSpecUpdateInfo &specUpdateInfo)
{
    (void)specUpdateInfo;
}

void
EStride::squash(ThreadID tid, const uint64_t seq_no)
{
    assertValidTid(tid);
    inflightWindows[tid].squash(seq_no);
}

}

}
