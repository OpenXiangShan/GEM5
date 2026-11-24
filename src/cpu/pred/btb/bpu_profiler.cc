#include "cpu/pred/btb/bpu_profiler.hh"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <string>
#include <tuple>

#include "base/output.hh"
#include "base/trace.hh"
#include "debug/Profiling.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

BPUProfiler::BPUProfiler(const std::string &name, unsigned maxInstsNum)
    : objectName(name), maxInstsNum(maxInstsNum)
{
    commitFtqEntryHasInstsVector.resize(maxInstsNum + 1, 0);
    lastPhaseFtqEntryNumCommittedInstDist.resize(maxInstsNum + 1, 0);
    commitFtqEntryFetchedInstsVector.resize(maxInstsNum + 1, 0);
    lastPhaseFtqEntryNumFetchedInstDist.resize(maxInstsNum + 1, 0);
}

bool
BPUProfiler::recordBranch(const FetchTarget &entry, Addr branchAddr,
                         const BranchInfo &info, bool taken, bool mispred)
{
    MispredType mispredType = FAKE_LAST;

    // Determine misprediction type only if there was a misprediction
    if (mispred) {
        if (!taken) {
            // Only conditional branches can be not-taken
            assert(info.isCond);
            mispredType = DIR_WRONG; // Direction was wrong
        } else {
            // Check if this branch was in the predicted BTB entries
            bool predBranchInBTB = false;
            for (const auto pc : entry.predictedBranchPCs) {
                if (pc == branchAddr) {
                    predBranchInBTB = true;
                    break;
                }
            }

            if (!predBranchInBTB) {
                mispredType = NO_PRED; // Branch wasn't predicted at all
            } else if (entry.predTaken && entry.predBranchInfo.pc == branchAddr) {
                mispredType = TARGET_WRONG; // Branch predicted taken but wrong target
            } else {
                // Branch predicted not taken or different branch predicted taken
                mispredType = DIR_WRONG;
            }
        }

        DPRINTF(Profiling, "branchAddr %#lx is mispredicted, taken %d, type %d, missType %d\n",
                branchAddr, taken, info.getType(), mispredType);
        assert(mispredType != FAKE_LAST);
    }

    // Create branch key for the statistics map
    auto branchKey = std::make_pair(branchAddr, info.getType());

    // Update branch statistics
    DPRINTF(Profiling, "lookup topMispredictsByBranch for branchAddr %#lx, type %d\n",
            branchAddr, info.getType());

    auto statsIt = topMispredictsByBranch.find(branchKey);
    const bool firstObservation = statsIt == topMispredictsByBranch.end();

    if (statsIt == topMispredictsByBranch.end()) {
        // Create new statistics entry for this branch
        DPRINTF(Profiling, "not found, insert with mispred=%d\n", mispred);

        // Initialize new branch stats
        BranchStats stats(branchAddr, info.getType());
        stats.incrementTotal();  // Always increment total count

        // Only increment misprediction count if actually mispredicted
        if (mispred) {
            stats.incrementMispred(mispredType);
        }

        // Store in map
        topMispredictsByBranch[branchKey] = stats;
    } else {
        // Update existing statistics entry
        DPRINTF(Profiling, "found, total %d, miss %d\n",
                statsIt->second.totalCount, statsIt->second.mispredCount);

        // Always increment total count
        statsIt->second.incrementTotal();

        // Only increment misprediction count if actually mispredicted
        if (mispred) {
            statsIt->second.incrementMispred(mispredType);
        }
    }

    if (taken) {
        trackTakenBranch(branchAddr);
    }
    return firstObservation;
}

bool
BPUProfiler::recordFetchBlock(const FetchTarget &target,
                             const PreparedUpdate &update)
{
    bool firstBTBStart = false;

    // Check if this target was mispredicted
    const bool miss_predicted = update.outcome.controlMispred;
    // Track indirect mispredictions
    if (miss_predicted && update.outcome.branch.isIndirect) {
        topMispredIndirect[target.startPC]++;
    }

    if ((target.isHit || update.outcome.taken) && update.outcome.valid) {
        const BTBEntry btb_entry(update.outcome.branch);
        // Update BTB entry statistics
        auto it = totalBTBEntries.find(target.startPC);
        if (it == totalBTBEntries.end()) {
            totalBTBEntries[target.startPC] = std::make_pair(btb_entry, 1);
            firstBTBStart = true;
        } else {
            it->second.second++;
            it->second.first = btb_entry;
        }
    }

    if (target.commitInstNum >= 0 && target.commitInstNum <= maxInstsNum) {
        commitFtqEntryHasInstsVector[target.commitInstNum]++;
    }
    if (target.fetchInstNum >= 0 && target.fetchInstNum <= maxInstsNum) {
        commitFtqEntryFetchedInstsVector[target.fetchInstNum]++;
    }

    // --- Misprediction Statistics ---
    // Track control squashes (mispredictions)
    if (update.outcome.controlMispred) {
        // Record mispredict pair (start PC, branch PC)
        auto find_it = topMispredicts.find(
            std::make_pair(target.startPC, update.outcome.branch.pc));
        if (find_it == topMispredicts.end()) {
            topMispredicts[
                std::make_pair(target.startPC, update.outcome.branch.pc)] = 1;
        } else {
            find_it->second++;
        }

        // Track history pattern for mispredictions
        auto hist(target.history);
        hist.resize(18);
        uint64_t pattern = hist.to_ulong();
        auto find_it_hist = topMispredHist.find(pattern);
        if (find_it_hist == topMispredHist.end()) {
            topMispredHist[pattern] = 1;
        } else {
            find_it_hist->second++;
        }
    }

    return firstBTBStart;
}

void
BPUProfiler::recordInstructionCommit()
{
    ++numInstCommitted;

    // Completed windows have zero-based IDs in the CSV files.
    if (numInstCommitted % PhaseSize == 0) {
        const auto phaseID = numInstCommitted / PhaseSize - 1;
        DPRINTF(Profiling, "dump main phase %d\n", phaseID + 1);
        summarizeBranches(phaseID, lastPhaseTopMispredictsByBranch,
                          currentPhaseTakenBranches, phaseBranchRows);

        auto &committedDist = ftqEntryNumCommittedInstDistByPhase.emplace_back();
        auto &fetchedDist = ftqEntryNumFetchedInstDistByPhase.emplace_back();
        processFetchDistributions(committedDist, fetchedDist);
        summarizeBtbEntries(phaseID);
    }

    if (numInstCommitted % SubPhaseSize == 0) {
        const auto phaseID = numInstCommitted / SubPhaseSize - 1;
        DPRINTF(Profiling, "dump sub phase %d\n", phaseID + 1);
        summarizeBranches(phaseID, lastSubPhaseTopMispredictsByBranch,
                          currentSubPhaseTakenBranches, subPhaseBranchRows);
    }
}

void
BPUProfiler::processFetchDistributions(
    std::vector<int> &currentPhaseCommittedDist,
    std::vector<int> &currentPhaseFetchedDist)
{
    // Initialize distributions with zeros
    currentPhaseCommittedDist.resize(maxInstsNum+1, 0);
    currentPhaseFetchedDist.resize(maxInstsNum+1, 0);

    // Calculate the difference between current and last phase values
    for (int i = 0; i <= maxInstsNum; i++) {
        currentPhaseCommittedDist[i] = commitFtqEntryHasInstsVector[i] -
                                     lastPhaseFtqEntryNumCommittedInstDist[i];
        lastPhaseFtqEntryNumCommittedInstDist[i] = commitFtqEntryHasInstsVector[i];

        currentPhaseFetchedDist[i] = commitFtqEntryFetchedInstsVector[i] -
                                   lastPhaseFtqEntryNumFetchedInstDist[i];
        lastPhaseFtqEntryNumFetchedInstDist[i] = commitFtqEntryFetchedInstsVector[i];
    }
}

void
BPUProfiler::summarizeBtbEntries(uint64_t phaseID)
{
    std::unordered_map<Addr, std::pair<BTBEntry, int>> currentPhaseBTBEntries;

    // Process each BTB entry
    for (auto &it : totalBTBEntries) {
        auto &entry = it.second.first;
        auto visit_cnt = it.second.second;

        // Check if this entry was already present in last phase
        auto last_it = lastPhaseBTBEntries.find(it.first);
        if (last_it != lastPhaseBTBEntries.end()) {
            visit_cnt -= last_it->second.second;
        }

        // Only add entries with new visits in this phase
        if (visit_cnt > 0) {
            currentPhaseBTBEntries[it.first] = std::make_pair(entry, visit_cnt);
        }
    }

    // Update last phase BTB entries for next time
    lastPhaseBTBEntries = totalBTBEntries;

    // Keep the original hash iteration order as input to the sort: equal
    // visit counts otherwise change which entries appear in the report.
    std::vector<std::tuple<Addr, BTBEntry, int>> entries;
    entries.reserve(currentPhaseBTBEntries.size());
    for (const auto &[pc, entry] : currentPhaseBTBEntries) {
        entries.emplace_back(pc, entry.first, entry.second);
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return std::get<2>(a) > std::get<2>(b);
    });

    std::ostringstream row;
    row << phaseID << "," << entries.size();
    for (size_t i = 0; i < std::min(TopBtbEntries, entries.size()); ++i) {
        const auto &[pc, entry, count] = entries[i];
        row << "," << std::hex << pc << "," << std::dec << entry.branch.getType();
    }
    row << '\n';
    phaseBtbRows += row.str();
}

void
BPUProfiler::summarizeBranches(uint64_t phaseID, BranchStatsMap &lastStats,
                               std::unordered_map<Addr, int> &takenBranches,
                               std::string &rows)
{
    std::vector<std::pair<BranchKey, int>> records;
    int totalMispredicts = 0;
    for (const auto &[key, stats] : topMispredictsByBranch) {
        const auto last = lastStats.find(key);
        int misses = stats.mispredCount;
        if (last != lastStats.end()) {
            if (stats.totalCount <= last->second.totalCount) {
                continue;
            }
            misses -= last->second.mispredCount;
        }
        records.emplace_back(key, misses);
        totalMispredicts += misses;
    }

    // Input is in BranchKey order, including executed branches with no misses.
    std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });

    std::ostringstream row;
    row << phaseID << "," << records.size() << "," << takenBranches.size()
        << "," << totalMispredicts;
    for (size_t i = 0; i < std::min(TopBranches, records.size()); ++i) {
        const auto &[key, misses] = records[i];
        row << "," << std::hex << key.first << "," << std::dec << key.second
            << "," << misses;
    }
    row << '\n';
    rows += row.str();

    lastStats = topMispredictsByBranch;
    takenBranches.clear();
}

void
BPUProfiler::trackTakenBranch(Addr branchAddr)
{
    // Helper function to update a branch map
    auto updateBranchMap = [branchAddr](std::unordered_map<Addr, int> &branchMap) {
        auto it = branchMap.find(branchAddr);
        if (it == branchMap.end()) {
            // Branch not found - add with count 1
            branchMap[branchAddr] = 1;
        } else {
            // Branch found - increment count
            it->second++;
        }
    };

    // Track taken branches independently for main and sub-phases.
    updateBranchMap(currentPhaseTakenBranches);
    updateBranchMap(currentSubPhaseTakenBranches);
}

void
BPUProfiler::dump()
{
    // Helper function: create output file and write header
    auto createOutputFile = [](const std::string& filename, const std::string& header) {
        auto handle = simout.create(filename, false, true);
        *handle->stream() << header << std::endl;
        return handle;
    };

    // Generic sort function by key
    auto sortByKey = [](auto& data, auto keyFn) {
        std::sort(data.begin(), data.end(),
            [&keyFn](const auto& a, const auto& b) {
                return keyFn(a) > keyFn(b);
            });
    };

    // 1. Output top mispredictions
    auto outFile = createOutputFile("topMisPredicts.csv", "startPC,control_pc,count");
    // topMisPredPC: Vector of mispredict records (startPC, controlPC) -> count
    // startPC: Starting address of fetch block
    // controlPC: Address of branch instruction
    // count: Number of mispredictions
    std::vector<std::pair<std::pair<Addr, Addr>, int>> topMisPredPC(
        topMispredicts.begin(), topMispredicts.end());

    sortByKey(topMisPredPC, [](const auto& entry) { return entry.second; });
    for (const auto& entry : topMisPredPC) {
        *outFile->stream() << std::hex << entry.first.first << ","
            << entry.first.second << ","
            << std::dec << entry.second << std::endl;
    }
    simout.close(outFile);

    // 2. Output mispredictions by branch
    outFile = createOutputFile("topMispredictsByBranch.csv",
                "pc,type,mispredicts,total,misPermil,dirMiss,tgtMiss,noPredMiss");

    // topMisPredPCByBranch: Detailed misprediction records per branch
    std::vector<std::tuple<Addr, int, int, int, double, int, int, int>> topMisPredPCByBranch;
    for (const auto &it : topMispredictsByBranch) {
        const auto &stats = it.second;
        topMisPredPCByBranch.push_back(std::make_tuple(
            stats.pc, stats.branchType,
            stats.mispredCount, stats.totalCount,
            stats.getMispredRate(),
            stats.dirWrongCount, stats.targetWrongCount, stats.noPredCount));
    }

    sortByKey(topMisPredPCByBranch,
        [](const auto& entry) {
            return std::get<2>(entry);  // Sort by mispredCount
        });

    for (const auto& entry : topMisPredPCByBranch) {
        *outFile->stream()
            << std::hex << std::get<0>(entry) << ","
            << std::dec << std::get<1>(entry) << ","
            << std::get<2>(entry) << ","
            << std::get<3>(entry) << ","
            << std::get<4>(entry) << ","
            << std::get<5>(entry) << ","
            << std::get<6>(entry) << ","
            << std::get<7>(entry) << std::endl;
    }
    simout.close(outFile);

    // 3. Sort branches by misrate (per-mille)
    outFile = createOutputFile("topMisrateByBranch.csv",
                "pc,type,mispredicts,total,misPermil,dirMiss,tgtMiss,noPredMiss");

    // Reuse previous data, but sort by misrate
    int mispCntThres = 100;
    sortByKey(topMisPredPCByBranch, [](const auto& entry) { return std::get<4>(entry); });
    for (const auto& entry : topMisPredPCByBranch) {
        if (std::get<3>(entry) < mispCntThres) continue;

        *outFile->stream() << std::hex << std::get<0>(entry) << std::dec
            << "," << std::get<1>(entry)
            << "," << std::get<2>(entry)
            << "," << std::get<3>(entry)
            << "," << (int)std::get<4>(entry)
            << "," << (int)std::get<5>(entry)
            << "," << (int)std::get<6>(entry)
            << "," << (int)std::get<7>(entry) << std::endl;
    }
    simout.close(outFile);

    auto writeBranchPhases = [&](const std::string &filename,
                                 const char *idColumn, const std::string &rows) {
        outFile = simout.create(filename, false, true);
        auto &out = *outFile->stream();
        out << idColumn << ",numBranches,numEverTakenBranches,totalMispredicts";
        for (size_t i = 0; i < TopBranches; ++i) {
            out << ",topMispPC_" << i << ",type_" << i << ",misCnt_" << i;
        }
        out << '\n' << rows;
        simout.close(outFile);
    };
    writeBranchPhases("topMispredictByPhase.csv", "phaseID", phaseBranchRows);
    writeBranchPhases("topMispredictBySubPhase.csv", "subPhaseID",
                      subPhaseBranchRows);

    // 5. Output history misprediction data
    outFile = createOutputFile("topMisPredictHist.csv", "Hist,count");
    // Vector of (history pattern, count) pairs
    std::vector<std::pair<uint64_t, uint64_t>>
        topMisPredHistVec(topMispredHist.begin(), topMispredHist.end());

    sortByKey(topMisPredHistVec, [](const auto& entry) { return entry.second; });
    for (const auto& entry : topMisPredHistVec) {
        *outFile->stream() << std::hex << entry.first << ","
            << std::dec << entry.second << std::endl;
    }
    simout.close(outFile);

    // 6. Output indirect mispredictions
    outFile = createOutputFile("misPredIndirectStream.csv", "count,address");
    // Vector of (address, count) pairs for indirect branches
    std::vector<std::pair<Addr, unsigned>>
        indirectVec(topMispredIndirect.begin(), topMispredIndirect.end());

    sortByKey(indirectVec, [](const auto& entry) { return entry.second; });
    for (const auto& entry : indirectVec) {
        *outFile->stream() << std::oct << entry.second << ","
            << std::hex << entry.first << std::endl;
    }
    simout.close(outFile);

    // Process FTQ instruction distributions.
    auto processFtqDistribution = [&](const std::string& filename,
                                  const auto& distByPhase) {
        outFile = simout.create(filename, false, true);
        auto& out = *outFile->stream();

        // Write header
        out << "phaseID";
        for (int i = 0; i <= maxInstsNum; i++) {
            out << "," << i;
        }
        out << ",average" << std::endl;

        // Write data for each phase
        int phaseID = 0;
        for (const auto& dist : distByPhase) {
            out << phaseID;

            int numFtqEntries = 0;
            for (int i = 0; i <= maxInstsNum; i++) {
                numFtqEntries += dist[i];
            }

            for (int i = 0; i <= maxInstsNum; i++) {
                out << "," << dist[i];
            }

            out << "," << (double)PhaseSize / (double)numFtqEntries << std::endl;
            phaseID++;
        }

        simout.close(outFile);
    };

    // 7-8. Output FTQ distribution data
    processFtqDistribution("ftqEntryCommittedInstNumDistsByPhase.csv",
                          ftqEntryNumCommittedInstDistByPhase);

    processFtqDistribution("ftqEntryFetchedInstNumDistsByPhase.csv",
                          ftqEntryNumFetchedInstDistByPhase);

    // 9. Output BTB entries
    std::ostringstream header;
    header << "phaseID,numBTBEntries";
    for (size_t i = 0; i < TopBtbEntries; ++i) {
        header << ",entry_" << i << "_pc,entry_" << i << "_type";
    }
    outFile = createOutputFile("btbEntriesByPhase.csv", header.str());
    *outFile->stream() << phaseBtbRows;
    simout.close(outFile);
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
