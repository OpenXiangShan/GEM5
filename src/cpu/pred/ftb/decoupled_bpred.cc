#include "cpu/pred/ftb/decoupled_bpred.hh"

#include "base/output.hh"
#include "base/debug_helper.hh"
// #include "cpu/base.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/ftb/stream_common.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/DecoupleBPHist.hh"
#include "debug/Override.hh"
#include "debug/FTB.hh"
#include "debug/FTBITTAGE.hh"
#include "debug/JumpAheadPredictor.hh"
#include "debug/Profiling.hh"
#include "sim/core.hh"

namespace gem5
{
namespace branch_prediction
{
namespace ftb_pred
{

DecoupledBPUWithFTB::DecoupledBPUWithFTB(const DecoupledBPUWithFTBParams &p)
    : BPredUnit(p),
      enableLoopBuffer(p.enableLoopBuffer),
      enableLoopPredictor(p.enableLoopPredictor),
      enableJumpAheadPredictor(p.enableJumpAheadPredictor),
      enableTwoTaken(p.enableTwoTaken),
      fetchTargetQueue(p.ftq_size),
      fetchStreamQueueSize(p.fsq_size),
      numBr(p.numBr),
      historyBits(p.maxHistLen),
      uftb(p.uftb),
      ftb(p.ftb),
      tage(p.tage),
      ittage(p.ittage),
      ras(p.ras),
      uras(p.uras),
    //   enableDB(p.enableBPDB),
      bpDBSwitches(p.bpDBSwitches),
      numStages(p.numStages),
      historyManager(p.numBr),
      dbpFtbStats(this, p.numStages, p.fsq_size)
{
    if (bpDBSwitches.size() > 0) {
        
        bpdb.init_db();
        enableBranchTrace = checkGivenSwitch(bpDBSwitches, std::string("basic"));
        if (enableBranchTrace) {
            std::vector<std::pair<std::string, DataType>> fields_vec = {
                std::make_pair("startPC", UINT64),
                std::make_pair("controlPC", UINT64),
                std::make_pair("controlType", UINT64),
                std::make_pair("taken", UINT64),
                std::make_pair("mispred", UINT64),
                std::make_pair("fallThruPC", UINT64),
                std::make_pair("source", UINT64),
                std::make_pair("target", UINT64)
            };
            bptrace = bpdb.addAndGetTrace("BPTRACE", fields_vec);
            bptrace->init_table(); 
            removeGivenSwitch(bpDBSwitches, std::string("basic"));
            someDBenabled = true;
        }

        // check whether "loop" is in bpDBSwitches
        enableLoopDB = checkGivenSwitch(bpDBSwitches, std::string("loop"));
        if (enableLoopDB) {
            std::vector<std::pair<std::string, DataType>> loop_fields_vec = {
                std::make_pair("pc", UINT64),
                std::make_pair("target", UINT64),
                std::make_pair("mispred", UINT64),
                std::make_pair("training", UINT64),
                std::make_pair("trainSpecCnt", UINT64),
                std::make_pair("trainTripCnt", UINT64),
                std::make_pair("trainConf", UINT64),
                std::make_pair("inMain", UINT64),
                std::make_pair("mainTripCnt", UINT64),
                std::make_pair("mainConf", UINT64),
                std::make_pair("predSpecCnt", UINT64),
                std::make_pair("predTripCnt", UINT64),
                std::make_pair("predConf", UINT64)
            };
            lptrace = bpdb.addAndGetTrace("LOOPTRACE", loop_fields_vec);
            lptrace->init_table();
            removeGivenSwitch(bpDBSwitches, std::string("loop"));
            someDBenabled = true;
        }
    }

    bpType = DecoupledFTBType;
    numStages = 3;
    // TODO: better impl (use vector to assign in python)
    // problem: ftb->getAndSetNewFTBEntry
    components.push_back(uftb);
    components.push_back(uras);
    components.push_back(ftb);
    components.push_back(tage);
    components.push_back(ras);
    components.push_back(ittage);
    numComponents = components.size();
    for (int i = 0; i < numComponents; i++) {
        components[i]->setComponentIdx(i);
        if (components[i]->hasDB) {
            bool enableDB = checkGivenSwitch(bpDBSwitches, components[i]->dbName);
            if (enableDB) {
                components[i]->enableDB = true;
                components[i]->setDB(&bpdb);
                components[i]->setTrace();
                removeGivenSwitch(bpDBSwitches, components[i]->dbName);
                someDBenabled = true;
            }
        }
    }
    if (bpDBSwitches.size() > 0) {
        warn("bpDBSwitches contains unknown switches\n");
        printf("unknown switches: ");
        for (auto it = bpDBSwitches.begin(); it != bpDBSwitches.end(); it++) {
            printf("%s ", it->c_str());
        }
        printf("\n");
    }

    predsOfEachStage.resize(numStages);
    for (unsigned i = 0; i < numStages; i++) {
        predsOfEachStage[i].predSource = i;
        predsOfEachStage[i].condTakens.resize(numBr, false);
    }

    s0PC = 0x80000000;

    s0History.resize(historyBits, 0);
    fetchTargetQueue.setName(name());

    commitHistory.resize(historyBits, 0);
    squashing = true;

    lp = LoopPredictor(16, 4, enableLoopDB);
    lb.setLp(&lp);

    jap = JumpAheadPredictor(16, 4);

    if (!enableLoopPredictor && enableLoopBuffer) {
        fatal("loop buffer cannot be enabled without loop predictor\n");
    }
    commitFsqEntryHasInstsVector.resize(16+1, 0);
    lastPhaseFsqEntryNumCommittedInstDist.resize(16+1, 0);
    commitFsqEntryFetchedInstsVector.resize(16+1, 0);
    lastPhaseFsqEntryNumFetchedInstDist.resize(16+1, 0);

    registerExitCallback([this]() {
        auto out_handle = simout.create("topMisPredicts.txt", false, true);
        *out_handle->stream() << "startPC" << " " << "control pc" << " " << "count" << std::endl;
        std::vector<std::pair<std::pair<Addr, Addr>, int>> topMisPredPC;
        for (auto &it : topMispredicts) {
            topMisPredPC.push_back(it);
        }
        std::sort(topMisPredPC.begin(), topMisPredPC.end(), [](const std::pair<std::pair<Addr, Addr>, int> &a, const std::pair<std::pair<Addr, Addr>, int> &b) {
            return a.second > b.second;
        });
        for (auto& it : topMisPredPC) {
            *out_handle->stream() << std::hex << it.first.first << " " << it.first.second << " " << std::dec << it.second << std::endl;
        }
        simout.close(out_handle);

        // at a per branch basis
        out_handle = simout.create("topMispredictsByBranch.txt", false, true);
        std::vector<std::tuple<Addr, int, int, int, double, int, int, int>> topMisPredPCByBranch;
        *out_handle->stream() << "pc" << " " << "type" << " " << "mispredicts" << " " << "total" << " " << "misPermil" << " " << "dirMiss" << " " << "tgtMiss" << " " << "noPredMiss" << std::endl;
        for (auto &it : topMispredictsByBranch) {
            topMisPredPCByBranch.push_back(std::make_tuple(
                it.first.first, it.first.second, it.second.first.first, it.second.second,
                (double)(it.second.first.first * 1000) / (double)it.second.second,
                it.second.first.second.at(DIR_WRONG), it.second.first.second.at(TARGET_WRONG), it.second.first.second.at(NO_PRED)));
        }
        std::sort(topMisPredPCByBranch.begin(), topMisPredPCByBranch.end(), [](
            const std::tuple<Addr, int, int, int, double, int, int, int> &a,
            const std::tuple<Addr, int, int, int, double, int, int, int> &b) {
                return std::get<2>(a) > std::get<2>(b);
        });
        for (auto& it : topMisPredPCByBranch) {
            *out_handle->stream() << std::hex << std::get<0>(it) << std::dec << " " << std::get<1>(it) << " " << std::get<2>(it) << " " << std::get<3>(it);
            *out_handle->stream() << " " << (int)std::get<4>(it) << " " << (int)std::get<5>(it) << " " << (int)std::get<6>(it) << " " << (int)std::get<7>(it) << std::endl;
        }
        simout.close(out_handle);

        // top misrate branches
        out_handle = simout.create("topMisrateByBranch.txt", false, true);
        // sort by misrate (permil), filter by total count
        *out_handle->stream() << "pc" << " " << "type" << " " << "mispredicts" << " " << "total" << " " << "misPermil" << " " << "dirMiss" << " " << "tgtMiss" << " " << "noPredMiss" << std::endl;
        std::sort(topMisPredPCByBranch.begin(), topMisPredPCByBranch.end(), [](
            const std::tuple<Addr, int, int, int, double, int, int, int> &a,
            const std::tuple<Addr, int, int, int, double, int, int, int> &b) {
                return std::get<4>(a) > std::get<4>(b);
        });

        int mispCntThres = 100;
        for (auto& it : topMisPredPCByBranch) {
            if (std::get<3>(it) < mispCntThres) {
                continue;
            }
            *out_handle->stream() << std::hex << std::get<0>(it) << std::dec << " " << std::get<1>(it) << " " << std::get<2>(it) << " " << std::get<3>(it);
            *out_handle->stream() << " " << (int)std::get<4>(it) << " " << (int)std::get<5>(it) << " " << (int)std::get<6>(it) << " " << (int)std::get<7>(it) << std::endl;
        }
        simout.close(out_handle);

        int phaseID = 0;
        int outputTopN = 5;
        out_handle = simout.create("topMispredictByPhase.txt", false, true);
        *out_handle->stream() << "phaseID" << " " << "numBranches" << " " << "numEverTakenBranches" << " " << "totalMispredicts";
        for (int i = 0; i < outputTopN; i++) {
            *out_handle->stream() << " " << "topMispPC_" << i;
            *out_handle->stream() << " " << "type_" << i;
            *out_handle->stream() << " " << "misCnt_" << i;
            // *out_handle->stream() << " " << "topReason" << i;
        }
        *out_handle->stream()<< std::endl;
        for (auto& it : topMispredictsByBranchByPhase) {
            int numStaticBranches = it.size();
            int numEverTakenStaticBranches = takenBranchesByPhase[phaseID].size();
            int totalMispredicts = 0;
            std::vector<MispredRecord> temp;
            for (auto& it2 : it) {
                temp.push_back(it2);
            }
            for (auto& it2 : temp) {
                totalMispredicts += getMispredCount(it2);
            }
            *out_handle->stream() << phaseID << " " << numStaticBranches << " " << numEverTakenStaticBranches << " " << totalMispredicts;
            // sort by mispredicts
            std::sort(temp.begin(), temp.end(), [&](const MispredRecord &a, const MispredRecord &b) {
                return gem5::branch_prediction::ftb_pred::DecoupledBPUWithFTB::getMispredCount(a) >
                gem5::branch_prediction::ftb_pred::DecoupledBPUWithFTB::getMispredCount(b);
            });
            for (int i = 0; i < outputTopN && i < temp.size(); i++) {
                *out_handle->stream() << " " << std::hex << temp[i].first.first; // pc
                *out_handle->stream() << " " << std::dec << temp[i].first.second; // type
                *out_handle->stream() << " " << std::dec << getMispredCount(temp[i]); // mispred count
                // *out_handle->stream() << " " << temp[i].first.first;
            }
            *out_handle->stream() << std::dec << std::endl;
            phaseID++;
        }
        simout.close(out_handle);

        phaseID = 0;
        out_handle = simout.create("topMispredictBySubPhase.txt", false, true);
        *out_handle->stream() << "subPhaseID" << " " << "numBranches" << " " << "numEverTakenBranches" << " " << "totalMispredicts";
        for (int i = 0; i < outputTopN; i++) {
            *out_handle->stream() << " " << "topMispPC_" << i;
            *out_handle->stream() << " " << "type_" << i;
            *out_handle->stream() << " " << "misCnt_" << i;
        }
        *out_handle->stream()<< std::endl;
        for (auto& it : topMispredictsByBranchBySubPhase) {
            int numStaticBranches = it.size();
            int numEverTakenStaticBranches = takenBranchesBySubPhase[phaseID].size();
            int totalMispredicts = 0;
            std::vector<MispredRecord> temp;
            for (auto& it2 : it) {
                temp.push_back(it2);
            }
            for (auto& it2 : temp) {
                totalMispredicts += getMispredCount(it2);
            }
            *out_handle->stream() << phaseID << " " << numStaticBranches << " " << numEverTakenStaticBranches << " " << totalMispredicts;
            // sort by mispredicts
            std::sort(temp.begin(), temp.end(), [&](const MispredRecord &a, const MispredRecord &b) {
                return gem5::branch_prediction::ftb_pred::DecoupledBPUWithFTB::getMispredCount(a) >
                gem5::branch_prediction::ftb_pred::DecoupledBPUWithFTB::getMispredCount(b);
            });
            *out_handle->stream() << std::hex;
            for (int i = 0; i < outputTopN && i < temp.size(); i++) {
                *out_handle->stream() << " " << std::hex << temp[i].first.first; // pc
                *out_handle->stream() << " " << std::dec << temp[i].first.second; // type
                *out_handle->stream() << " " << std::dec << getMispredCount(temp[i]); // mispred count
                // *out_handle->stream() << " " << temp[i].first.first;
            }
            *out_handle->stream() << std::dec << std::endl;
            phaseID++;
        }
        simout.close(out_handle);


        out_handle = simout.create("topMisPredictHist.txt", false, true);
        // *out_handle->stream() << "use loop but invalid: " << useLoopButInvalid 
        //                       << " use loop and valid: " << useLoopAndValid 
        //                       << " not use loop: " << notUseLoop << std::endl;
        *out_handle->stream() << "Hist" << " " << "count" << std::endl;
        std::vector<std::pair<uint64_t, uint64_t>> topMisPredHistVec;
        for (const auto &entry: topMispredHist) {
            topMisPredHistVec.push_back(entry);
        }
        std::sort(topMisPredHistVec.begin(), topMisPredHistVec.end(),
                  [](const std::pair<uint64_t, uint64_t> &a,
                     const std::pair<uint64_t, uint64_t> &b) {
                      return a.second > b.second;
                  });
        for (const auto &entry: topMisPredHistVec) {
            *out_handle->stream() << std::hex << entry.first << " " << std::dec << entry.second << std::endl;
        }

        // if (dumpLoopPred) {
        //     out_handle = simout.create("misPredTripCount.txt", false, true);
        //     *out_handle->stream() << missCount << std::endl;
        //     for (const auto &entry : misPredTripCount) {
        //         *out_handle->stream()
        //             << entry.first << " " << entry.second << std::endl;
        //     }

        //     out_handle = simout.create("loopInfo.txt", false, true);
        //     for (const auto &entry : storedLoopStreams) {
        //         bool misPred = entry.second.squashType == SQUASH_CTRL;
        //         *out_handle->stream()
        //             << std::dec << "miss: " << misPred << " " << entry.first << " "
        //             << std::hex << entry.second.startPC << ", "
        //             << (misPred ? entry.second.exeBranchPC
        //                         : entry.second.predBranchPC)
        //             << "--->"
        //             << (misPred ? entry.second.exeTarget : entry.second.predTarget)
        //             << std::dec
        //             << " useLoopPred: " << entry.second.useLoopPrediction
        //             << " tripCount: " << entry.second.tripCount << std::endl;
        //     }
        // }

        // out_handle = simout.create("targets.txt", false, true);
        // for (const auto it : storeTargets) {
        //     *out_handle->stream() << std::hex << it << std::endl;
        // }

        out_handle = simout.create("misPredIndirectStream.txt", false, true);
        std::vector<std::pair<Addr, unsigned>> tempVec;
        for (auto &it : topMispredIndirect) {
            tempVec.push_back(std::make_pair(it.first, it.second));
        }
        std::sort(tempVec.begin(), tempVec.end(),
            [](const std::pair<Addr, unsigned> &a,
               const std::pair<Addr, unsigned> &b) {
                return a.second > b.second;
            });
        for (auto it : tempVec) {
            *out_handle->stream() << std::oct << it.second << " " << std::hex << it.first << std::endl;
        }

        simout.close(out_handle);

        // dump fsq entry committed insts
        out_handle = simout.create("fsqEntryCommittedInstNumDistsByPhase.txt", false, true);
        *out_handle->stream() << "phaseID";
        for (int i = 0; i <= 16; i++) {
            *out_handle->stream() << " " << i;
        }
        *out_handle->stream() << " " << "average" << std::endl;

        phaseID = 0;
        for (auto& it : fsqEntryNumCommittedInstDistByPhase) {
            *out_handle->stream() << phaseID;
            int numFsqEntries = 0;
            for (int i = 0; i <= 16; i++) {
                *out_handle->stream() << " " << it[i];
                numFsqEntries += it[i];
            }
            *out_handle->stream() << " " << (double)phaseSizeByInst / (double)numFsqEntries << std::endl;
            phaseID++;
        }
        simout.close(out_handle);

        // dump fsq entry fetched insts
        out_handle = simout.create("fsqEntryFetchedInstNumDistsByPhase.txt", false, true);
        *out_handle->stream() << "phaseID";
        for (int i = 0; i <= 16; i++) {
            *out_handle->stream() << " " << i;
        }
        *out_handle->stream() << " " << "average" << std::endl;
        phaseID = 0;
        for (auto& it : fsqEntryNumFetchedInstDistByPhase) {
            *out_handle->stream() << phaseID;
            int numFsqEntries = 0;
            for (int i = 0; i <= 16; i++) {
                *out_handle->stream() << " " << it[i];
                numFsqEntries += it[i];
            }
            *out_handle->stream() << " " << (double)phaseSizeByInst / (double)numFsqEntries << std::endl;
            phaseID++;
        }
        simout.close(out_handle);

        // dump ftb entries
        int outputTopNEntries = 1;
        out_handle = simout.create("ftbEntriesByPhase.txt", false, true);
        *out_handle->stream() << "phaseID"<< " " << "numFTBEntries";
        for (int i = 0; i <= outputTopNEntries; i++) {
            *out_handle->stream() << " " << "entry_" << i << "_pc";
            for (int n = 0; n < numBr; n++) {
                *out_handle->stream() << " " << "entry_" << i << "_br_" << n << "_pc";
                *out_handle->stream() << " " << "entry_" << i << "_br_" << n << "_type";
            }
        }
        *out_handle->stream() << std::endl;
        phaseID = 0;

        for (auto& it : FTBEntriesByPhase) {
            *out_handle->stream() << std::dec << phaseID;
            *out_handle->stream() << " " << it.size();
            std::vector<std::tuple<Addr, FTBEntry, int>> ftbEntryTempVec;
            for (auto& rec : it) {
                ftbEntryTempVec.push_back(std::make_tuple(rec.first, rec.second.first, rec.second.second));
            }
            std::sort(ftbEntryTempVec.begin(), ftbEntryTempVec.end(),
                [](const std::tuple<Addr, FTBEntry, int> &a,
                const std::tuple<Addr, FTBEntry, int> &b) {
                    return std::get<2>(a) > std::get<2>(b);
                });
            for (int i = 0; i <= outputTopNEntries && i < ftbEntryTempVec.size(); i++) {
                auto &rec = ftbEntryTempVec[i];
                *out_handle->stream() << " " << std::hex << std::get<0>(rec);
                auto &entry = std::get<1>(rec);
                for (int n = 0; n < numBr; n++) {
                    auto slot = entry.slots.size() <= n ? FTBSlot() : entry.slots[n];
                    *out_handle->stream() << " " << slot.pc;
                    *out_handle->stream() << " " << slot.getType();
                }
            }
            *out_handle->stream() << std::endl;
            phaseID++;
        }
        simout.close(out_handle);

        if (someDBenabled) {
            bpdb.save_db("bp.db");
        }
    });
}

DecoupledBPUWithFTB::DBPFTBStats::DBPFTBStats(statistics::Group* parent, unsigned numStages, unsigned fsqSize):
    statistics::Group(parent),
    ADD_STAT(condNum, statistics::units::Count::get(), "the number of cond branches"),
    ADD_STAT(uncondNum, statistics::units::Count::get(), "the number of uncond branches"),
    ADD_STAT(returnNum, statistics::units::Count::get(), "the number of return branches"),
    ADD_STAT(otherNum, statistics::units::Count::get(), "the number of other branches"),
    ADD_STAT(condMiss, statistics::units::Count::get(), "the number of cond branch misses"),
    ADD_STAT(uncondMiss, statistics::units::Count::get(), "the number of uncond branch misses"),
    ADD_STAT(returnMiss, statistics::units::Count::get(), "the number of return branch misses"),
    ADD_STAT(otherMiss, statistics::units::Count::get(), "the number of other branch misses"),
    ADD_STAT(staticBranchNum, statistics::units::Count::get(), "the number of all (different) static branches"),
    ADD_STAT(staticBranchNumEverTaken, statistics::units::Count::get(), "the number of all (different) static branches that are once taken"),
    ADD_STAT(predsOfEachStage, statistics::units::Count::get(), "the number of preds of each stage that account for final pred"),
    ADD_STAT(commitPredsFromEachStage, statistics::units::Count::get(), "the number of preds of each stage that account for a committed stream"),
    ADD_STAT(fsqEntryDist, statistics::units::Count::get(), "the distribution of number of entries in fsq"),
    ADD_STAT(fsqEntryEnqueued, statistics::units::Count::get(), "the number of fsq entries enqueued"),
    ADD_STAT(fsqEntryCommitted, statistics::units::Count::get(), "the number of fsq entries committed at last"),
    ADD_STAT(controlSquash, statistics::units::Count::get(), "the number of control squashes in bpu"),
    ADD_STAT(nonControlSquash, statistics::units::Count::get(), "the number of non-control squashes in bpu"),
    ADD_STAT(trapSquash, statistics::units::Count::get(), "the number of trap squashes in bpu"),
    ADD_STAT(ftqNotValid, statistics::units::Count::get(), "fetch needs ftq req but ftq not valid"),
    ADD_STAT(fsqNotValid, statistics::units::Count::get(), "ftq needs fsq req but fsq not valid"),
    ADD_STAT(fsqFullCannotEnq, statistics::units::Count::get(), "bpu has req but fsq full cannot enqueue"),
    ADD_STAT(fsqFullFetchHungry, statistics::units::Count::get(), "fetch hungry when fsq full and bpu cannot enqueue"),
    ADD_STAT(commitFsqEntryHasInsts, statistics::units::Count::get(), "number of insts that commit fsq entries have"),
    ADD_STAT(commitFsqEntryFetchedInsts, statistics::units::Count::get(), "number of insts that commit fsq entries fetched"),
    ADD_STAT(commitFsqEntryOnlyHasOneJump, statistics::units::Count::get(), "number of fsq entries with only one instruction (jump)"),
    ADD_STAT(ftbHit, statistics::units::Count::get(), "ftb hits (in predict block)"),
    ADD_STAT(ftbMiss, statistics::units::Count::get(), "ftb misses (in predict block)"),
    ADD_STAT(ftbMissInstNotCommitted, statistics::units::Count::get(), "inst causing ftb miss but not committed"),
    ADD_STAT(ftbMissInstNotMispredicted, statistics::units::Count::get(), "inst causing ftb miss but not mispredicted"),
    ADD_STAT(ftbMissInstMispredicted, statistics::units::Count::get(), "inst causing ftb miss actually mispredicted"),
    ADD_STAT(ftbMissWithNoMispreds, statistics::units::Count::get(), "ftb miss with no branch mispreds"),
    ADD_STAT(ftbMissWithNoBranches, statistics::units::Count::get(), "ftb miss with no branches committed"),
    ADD_STAT(ftbMissWithNoHarm, statistics::units::Count::get(), "not harmful ftb misses, miss with no actually taken branches in it"),
    ADD_STAT(ftbEntriesWithDifferentStart, statistics::units::Count::get(), "number of ftb entries with different start PC"),
    ADD_STAT(ftbEntriesWithOnlyOneJump, statistics::units::Count::get(), "number of ftb entries with different start PC starting with a jump"),
    ADD_STAT(predFalseHit, statistics::units::Count::get(), "false hit detected at pred"),
    ADD_STAT(commitFalseHit, statistics::units::Count::get(), "false hit detected at commit"),
    ADD_STAT(committedSquashedCondNotMispredicted, statistics::units::Count::get(), "control squashed cond branch but no commit mispreds recorded (should not happen)"),
    ADD_STAT(committedDecodeSquashedCondNotMispredicted, statistics::units::Count::get(), "control squashed (from decode) cond branch but no commit mispreds recorded (should not happen)"),
    ADD_STAT(committedCommitSquashedCondNotMispredicted, statistics::units::Count::get(), "control squashed (from commit) cond branchbut no commit mispreds recorded (should not happen)"),
    ADD_STAT(committedSquashedUncondNotMispredicted, statistics::units::Count::get(), "control squashed uncond branch but no commit mispreds recorded (possibly ftb unseen jals)"),
    ADD_STAT(committedDecodeSquashedUncondNotMispredicted, statistics::units::Count::get(), "control squashed (from decode) uncond branch but no commit mispreds recorded (possibly ftb unseen jals corrected by decode)"),
    ADD_STAT(committedCommitSquashedUncondNotMispredicted, statistics::units::Count::get(), "control squashed (from decode) uncond branch but no commit mispreds recorded (should not happen)"),
    ADD_STAT(committedSquashedBranchFinallyMispredicted, statistics::units::Count::get(), "control squashed branch finally mispredicted"),
    ADD_STAT(controlSquashedCommitted, statistics::units::Count::get(), "control squashed and committed insts"),
    ADD_STAT(controlDecodeSquashedCondCommitted, statistics::units::Count::get(), "control squashed from decode and committed conditional branches"),
    ADD_STAT(controlDecodeSquashedUncondCommitted, statistics::units::Count::get(), "control squashed from decode and committed unconditional branches"),
    ADD_STAT(controlDecodeSquashedUncondDirectCommitted, statistics::units::Count::get(), "control squashed from decode and committed unconditional direct branches"),
    ADD_STAT(controlDecodeSquashedUncondIndirectCommitted, statistics::units::Count::get(), "control squashed from decode and committed indirect branches"),
    ADD_STAT(controlDecodeSquashedUncondReturnCommitted, statistics::units::Count::get(), "control squashed from decode and committed return branches"),
    ADD_STAT(controlCommitSquashedCondCommitted, statistics::units::Count::get(), "control squashed from commit and committed conditional branches"),
    ADD_STAT(controlCommitSquashedUncondCommitted, statistics::units::Count::get(), "control squashed from commit and committed unconditional branches"),
    ADD_STAT(controlCommitSquashedUncondDirectCommitted, statistics::units::Count::get(), "control squashed from commit and committed unconditional direct branches"),
    ADD_STAT(controlCommitSquashedUncondIndirectCommitted, statistics::units::Count::get(), "control squashed from commit and committed indirect branches"),
    ADD_STAT(controlCommitSquashedUncondReturnCommitted, statistics::units::Count::get(), "control squashed from commit and committed return branches"),
    ADD_STAT(controlSquashedNotCommitted, statistics::units::Count::get(), "control squashed but not committed insts"),
    ADD_STAT(controlDecodeSquashedCondNotCommitted, statistics::units::Count::get(), "control squashed from decode but not committed conditional branches"),
    ADD_STAT(controlDecodeSquashedUncondNotCommitted, statistics::units::Count::get(), "control squashed from decode but not committed unconditional branches"),
    ADD_STAT(controlDecodeSquashedUncondDirectNotCommitted, statistics::units::Count::get(), "control squashed from decode but not committed unconditional direct branches"),
    ADD_STAT(controlDecodeSquashedUncondIndirectNotCommitted, statistics::units::Count::get(), "control squashed from decode but not committed indirect branches"),
    ADD_STAT(controlDecodeSquashedUncondReturnNotCommitted, statistics::units::Count::get(), "control squashed from decode but not committed return branches"),
    ADD_STAT(controlCommitSquashedCondNotCommitted, statistics::units::Count::get(), "control squashed from commit but not committed conditional branches"),
    ADD_STAT(controlCommitSquashedUncondNotCommitted, statistics::units::Count::get(), "control squashed from commit but not committed unconditional branches"),
    ADD_STAT(controlCommitSquashedUncondDirectNotCommitted, statistics::units::Count::get(), "control squashed from commit but not committed unconditional direct branches"),
    ADD_STAT(controlCommitSquashedUncondIndirectNotCommitted, statistics::units::Count::get(), "control squashed from commit but not committed indirect branches"),
    ADD_STAT(controlCommitSquashedUncondReturnNotCommitted, statistics::units::Count::get(), "control squashed from commit but not committed return branches"),
    // ADD_STAT(nonControlSquashedCommitted, statistics::units::Count::get(), "non-control squashed and committed insts"),
    // ADD_STAT(nonControlSquashedNotCommitted, statistics::units::Count::get(), "non-control squashed but not committed insts"),
    // ADD_STAT(trapSquashedCommitted, statistics::units::Count::get(), "trap squashed and committed insts"),
    // ADD_STAT(trapSquashedNotCommitted, statistics::units::Count::get(), "trap squashed but not committed insts"),
    ADD_STAT(committedStreamHadReceivedSquash, statistics::units::Count::get(), "committed stream had received squash"),
    ADD_STAT(committedStreamSquashFromDecode, statistics::units::Count::get(), "committed stream had received squash from decode"),
    ADD_STAT(committedStreamSquashFromCommit, statistics::units::Count::get(), "committed stream had received squash from commit"),
    ADD_STAT(predLoopPredictorExit, statistics::units::Count::get(), "loop predictor exits at pred"),
    ADD_STAT(predLoopPredictorUnconfNotExit, statistics::units::Count::get(), "loop predictor does not exit at pred because of unconf"),
    ADD_STAT(predLoopPredictorConfFixNotExit, statistics::units::Count::get(), "loop predictor confident and fix other predictor not taken in non-exit loop branch at pred"),
    ADD_STAT(predFTBUnseenLoopBranchInLp, statistics::units::Count::get(), "loop predictor recorded loop branch that is not in ftb encountered"),
    ADD_STAT(predFTBUnseenLoopBranchExitInLp, statistics::units::Count::get(), "loop predictor recorded loop branch that is not in ftb encountered and exited"),
    ADD_STAT(commitLoopPredictorExit, statistics::units::Count::get(), "loop predictor pred loop exits detected at commit"),
    ADD_STAT(commitLoopPredictorExitCorrect, statistics::units::Count::get(), "loop predictor correctly pred loop exits detected at commit"),
    ADD_STAT(commitLoopPredictorExitWrong, statistics::units::Count::get(), "loop predictor wrongly pred loop exits detected at commit"),
    ADD_STAT(commitFTBUnseenLoopBranchInLp, statistics::units::Count::get(), "loop predictor recorded loop branch that is not in ftb encountered at commit"),
    ADD_STAT(commitFTBUnseenLoopBranchExitInLp, statistics::units::Count::get(), "loop predictor recorded loop branch that is not in ftb encountered and exited at commit"),
    ADD_STAT(commitLoopPredictorConfFixNotExit, statistics::units::Count::get(), "loop predictor confident and fix other predictor not taken in non-exit loop branch at commit"),
    ADD_STAT(commitLoopPredictorConfFixNotExitCorrect, statistics::units::Count::get(), "loop predictor confident and fix other predictor not taken in non-exit loop branch correctly at commit"),
    ADD_STAT(commitLoopPredictorConfFixNotExitWrong, statistics::units::Count::get(), "loop predictor confident and fix other predictor not taken in non-exit loop branch wrongly at commit"),
    ADD_STAT(commitLoopExitLoopPredictorNotPredicted, statistics::units::Count::get(), "loop exit detected at commit that loop predictor did not pred exit"),
    ADD_STAT(commitLoopExitLoopPredictorNotConf, statistics::units::Count::get(), "loop exit detected at commit that loop predictor did not pred exit because of unconfident"),
    ADD_STAT(controlSquashOnLoopPredictorPredExit, statistics::units::Count::get(), "cotrol squash on loop predictor pred loop exits"),
    ADD_STAT(nonControlSquashOnLoopPredictorPredExit, statistics::units::Count::get(), "non-cotrol squash on loop predictor pred loop exits"),
    ADD_STAT(trapSquashOnLoopPredictorPredExit, statistics::units::Count::get(), "trap squash on loop predictor pred loop exits"),
    ADD_STAT(predBlockInLoopBuffer, statistics::units::Count::get(), "predicted block is from loop buffer"),
    ADD_STAT(predDoubleBlockInLoopBuffer, statistics::units::Count::get(), "predicted double block is from loop buffer"),
    ADD_STAT(squashOnLoopBufferPredBlock, statistics::units::Count::get(), "squash on loop buffer provided block"),
    ADD_STAT(squashOnLoopBufferDoublePredBlock, statistics::units::Count::get(), "squash on loop buffer provided double block"),
    ADD_STAT(commitBlockInLoopBuffer, statistics::units::Count::get(), "committed block is from loop buffer"),
    ADD_STAT(commitDoubleBlockInLoopBuffer, statistics::units::Count::get(), "committed double block is from loop buffer"),
    ADD_STAT(commitBlockInLoopBufferSquashed, statistics::units::Count::get(), "committed block is from loop buffer but squashed"),
    ADD_STAT(commitDoubleBlockInLoopBufferSquashed, statistics::units::Count::get(), "committed double block is from loop buffer but squashed"),
    ADD_STAT(commitLoopBufferEntryInstNum, statistics::units::Count::get(), "commit block from loop buffer, buffer entry has inst num"),
    ADD_STAT(commitLoopBufferDoubleEntryInstNum, statistics::units::Count::get(), "commit double block from loop buffer, buffer entry has inst num"),
    ADD_STAT(predJATotalSkippedBlocks, statistics::units::Count::get(), "jump ahead skipped total block numbers at pred"),
    ADD_STAT(commitJATotalSkippedBlocks, statistics::units::Count::get(), "jump ahead skipped total block numbers at commit"),
    ADD_STAT(squashOnJaHitBlocks, statistics::units::Count::get(), "total number of squashes on ja hit blocks"),
    ADD_STAT(controlSquashOnJaHitBlocks, statistics::units::Count::get(), "total number of control squashes on ja hit blocks"),
    ADD_STAT(nonControlSquashOnJaHitBlocks, statistics::units::Count::get(), "total number of non-control squashes on ja hit blocks"),
    ADD_STAT(trapSquashOnJaHitBlocks, statistics::units::Count::get(), "total number of trap squashes on ja hit blocks"),
    ADD_STAT(commitSquashedOnJaHitBlocks, statistics::units::Count::get(), "total number of squashes on ja hit committed blocks"),
    ADD_STAT(commitControlSquashedOnJaHitBlocks, statistics::units::Count::get(), "total number of control squashes on ja hit committed blocks"),
    ADD_STAT(commitNonControlSquashedOnJaHitBlocks, statistics::units::Count::get(), "total number of non-control squashes on ja hit committed blocks"),
    ADD_STAT(commitTrapSquashedOnJaHitBlocks, statistics::units::Count::get(), "total number of trap squashes on ja hit committed blocks"),
    ADD_STAT(predJASkippedBlockNum, statistics::units::Count::get(), "distribution of ja skipped block numbers at pred"),
    ADD_STAT(commitJASkippedBlockNum, statistics::units::Count::get(), "distribution of ja skipped block numbers at commit"),
    ADD_STAT(decodeControlSquashLatencyDist, statistics::units::Count::get(), "distribution of cycles count from pred to decode control squash"),
    ADD_STAT(commitControlSquashLatencyDist, statistics::units::Count::get(), "distribution of cycles count from pred to commit control squash"),
    ADD_STAT(commitTrapSquashLatencyDist, statistics::units::Count::get(), "distribution of cycles count from pred to commit trap squash"),
    ADD_STAT(commitNonControlSquashLatencyDist, statistics::units::Count::get(), "distribution of cycles count from pred to commit non-control squash"),
    ADD_STAT(updateLatencyDist, statistics::units::Count::get(), "distribution of cycles count from pred to commit update"),
    ADD_STAT(controlDecodeSquashOfCond, statistics::units::Count::get(), "control squash of cond branch at decode"),
    ADD_STAT(controlDecodeSquashOfUncond, statistics::units::Count::get(), "control squash of uncond branch at decode"),
    ADD_STAT(controlDecodeSquashOfUncondDirect, statistics::units::Count::get(), "control squash of uncond direct branch at decode"),
    ADD_STAT(controlDecodeSquashOfUncondIndirect, statistics::units::Count::get(), "control squash of indirect branch at decode"),
    ADD_STAT(controlDecodeSquashOfUncondReturn, statistics::units::Count::get(), "control squash of return branch at decode"),
    ADD_STAT(controlCommitSquashOfCond, statistics::units::Count::get(), "control squash of cond branch at commit"),
    ADD_STAT(controlCommitSquashOfUncond, statistics::units::Count::get(), "control squash of uncond branch at commit"),
    ADD_STAT(controlCommitSquashOfUncondDirect, statistics::units::Count::get(), "control squash of uncond direct branch at commit"),
    ADD_STAT(controlCommitSquashOfUncondIndirect, statistics::units::Count::get(), "control squash of indirect branch at commit"),
    ADD_STAT(controlCommitSquashOfUncondReturn, statistics::units::Count::get(), "control squash of return branch at commit")
{
    predsOfEachStage.init(numStages);
    commitPredsFromEachStage.init(numStages+1);
    fsqEntryDist.init(0, fsqSize, 1);
    commitLoopBufferEntryInstNum.init(0, 16, 1);
    commitLoopBufferDoubleEntryInstNum.init(0, 16, 1);
    commitFsqEntryHasInsts.init(0, 16, 1);
    commitFsqEntryFetchedInsts.init(0, 16, 1);
    predJASkippedBlockNum.init(0, 16, 1);
    commitJASkippedBlockNum.init(0, 16, 1);
    decodeControlSquashLatencyDist.init(1,16,1);
    commitControlSquashLatencyDist.init(1,16,1);
    commitTrapSquashLatencyDist.init(1,16,1);
    commitNonControlSquashLatencyDist.init(1,16,1);
    updateLatencyDist.init(1,64,2);
}

DecoupledBPUWithFTB::BpTrace::BpTrace(FetchStream &stream, const DynInstPtr &inst, bool mispred)
{
    _tick = curTick();
    Addr pc = inst->pcState().instAddr();
    const auto &rv_pc = inst->pcState().as<RiscvISA::PCState>();
    Addr target = rv_pc.npc();
    Addr fallThru = rv_pc.getFallThruPC();
    BranchInfo info(pc, target, inst->staticInst, fallThru-pc);
    set(stream.startPC, pc, info.getType(), inst->branching(), mispred, fallThru, stream.predSource, target);
    // for (auto it = _uint64_data.begin(); it != _uint64_data.end(); it++) {
    //     printf("%s: %ld\n", it->first.c_str(), it->second);
    // }
}

void
DecoupledBPUWithFTB::tick()
{
    dbpFtbStats.fsqEntryDist.sample(fetchStreamQueue.size(), 1);
    if (streamQueueFull()) {
        dbpFtbStats.fsqFullCannotEnq++;
    }


    int predsRemainsToBeMade = enableTwoTaken ? 2 : 1;
    // in two taken roofline model, we create max(bubblesOfPreds1, bubblesOfPreds2) bubbles
    int tempNumOverrideBubbles = 0;

    if (numOverrideBubbles > 0) {
        numOverrideBubbles--;
    }

    while (predsRemainsToBeMade > 0) {
        // make one prediction
        if (!squashing) {
            DPRINTF(DecoupleBP, "DecoupledBPUWithFTB::tick()\n");
            DPRINTF(Override, "DecoupledBPUWithFTB::tick()\n");
            tryEnqFetchTarget();
            tryEnqFetchStream();
        } else {
            receivedPred = false;
            DPRINTF(DecoupleBP, "Squashing, skip this cycle, receivedPred is %d.\n", receivedPred);
            DPRINTF(Override, "Squashing, skip this cycle, receivedPred is %d.\n", receivedPred);
        }


        sentPCHist = false;

        if (!receivedPred && !streamQueueFull()) {
            if (!enableLoopBuffer || (enableLoopBuffer && !lb.isActive())) {
                if (s0PC == ObservingPC) {
                    DPRINTFV(true, "Predicting block %#lx, id: %lu\n", s0PC, fsqId);
                }
                DPRINTF(DecoupleBP, "Requesting prediction for stream start=%#lx\n", s0PC);
                DPRINTF(Override, "Requesting prediction for stream start=%#lx\n", s0PC);
                // put startAddr in preds
                for (int i = 0; i < numStages; i++) {
                    predsOfEachStage[i].bbStart = s0PC;
                }
                for (int i = 0; i < numComponents; i++) {
                    components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);
                }
            } else {
                DPRINTF(LoopBuffer, "Do not query bpu when loop buffer is active\n");
                DPRINTF(DecoupleBP, "Do not query bpu when loop buffer is active\n");
            }


            sentPCHist = true;
        }


        // query loop buffer with start pc
        if (enableLoopBuffer && !lb.isActive() &&
                lb.streamBeforeLoop.getTakenTarget() == lb.streamBeforeLoop.startPC &&
                !lb.streamBeforeLoop.resolved) { // do not activate loop buffer right after squash
            lb.tryActivateLoop(s0PC);
        }

        DPRINTF(Override, "after putPCHistory\n");
        for (int i = 0; i < numStages; i++) {
            printFullFTBPrediction(predsOfEachStage[i]);
        }

        if (streamQueueFull()) {
            DPRINTF(DecoupleBP, "Stream queue is full, don't request prediction\n");
            DPRINTF(Override, "Stream queue is full, don't request prediction\n");
        }
        squashing = false;


        if (!receivedPred && numOverrideBubbles == 0 && sentPCHist) {
            tempNumOverrideBubbles = std::max(generateFinalPredAndCreateBubbles(), tempNumOverrideBubbles);
            if (!enableTwoTaken) {
                numOverrideBubbles = tempNumOverrideBubbles;
            } else {
                if (predsRemainsToBeMade == 1) {
                    numOverrideBubbles = tempNumOverrideBubbles;
                }
            }
        }

        predsRemainsToBeMade--;
    }
}

// this function collects predictions from all stages and generate bubbles
// when loop buffer is active, predictions are from saved stream
int
DecoupledBPUWithFTB::generateFinalPredAndCreateBubbles()
{
    int bubblesToCreate = 0;
    DPRINTF(Override, "In generateFinalPredAndCreateBubbles().\n");

    if (!enableLoopBuffer || (enableLoopBuffer && !lb.isActive())) {
        // predsOfEachStage should be ready now
        for (int i = 0; i < numStages; i++) {
            printFullFTBPrediction(predsOfEachStage[i]);
        }
        // choose the most accurate prediction
        FullFTBPrediction *chosen = &predsOfEachStage[0];

        for (int i = (int) numStages - 1; i >= 0; i--) {
            if (predsOfEachStage[i].valid) {
                chosen = &predsOfEachStage[i];
                DPRINTF(Override, "choose stage %d.\n", i);
                break;
            }
        }
        finalPred = *chosen;
        // calculate bubbles
        unsigned first_hit_stage = 0;
        while (first_hit_stage < numStages-1) {
            if (predsOfEachStage[first_hit_stage].match(*chosen)) {
                break;
            }
            first_hit_stage++;
        }
        // generate bubbles
        bubblesToCreate = first_hit_stage;
        // assign pred source
        finalPred.predSource = first_hit_stage;
        receivedPred = true;

        finalPred.predCycle = curCycle();

        // if stage 2 has ret (handled by main RAS)
        // check if we have handled in stage 1
        auto taken_slot_s1 = predsOfEachStage[1].getTakenSlot();
        if (taken_slot_s1.isReturn) {
            for (int i = 0; i < predsOfEachStage[1].ftbEntry.slots.size(); i++) {
                DPRINTF(FTBuRAS, "s2 slot valid %d isCond %d\n", predsOfEachStage[1].ftbEntry.slots[i].valid, predsOfEachStage[1].ftbEntry.slots[i].isCond);
            }
            DPRINTF(FTBuRAS, "pred to be ret on s2 with choose stage %d\n", first_hit_stage);
            DPRINTF(FTBuRAS, "s1 ftbEntry valid %d\n", predsOfEachStage[0].ftbEntry.valid);
            auto taken_slot_s0 = predsOfEachStage[0].getTakenSlot();
            for (int i = 0; i < predsOfEachStage[0].ftbEntry.slots.size(); i++) {
                DPRINTF(FTBuRAS, "s1 slot valid %d isCond %d\n", predsOfEachStage[0].ftbEntry.slots[i].valid, predsOfEachStage[0].ftbEntry.slots[i].isCond);
            }
            if (!taken_slot_s0.isReturn) {
                DPRINTF(FTBuRAS, "pc %llx pred to be ret on s2, but not on s1\n", predsOfEachStage[1].bbStart);
            }
            if (predsOfEachStage[1].returnTarget != predsOfEachStage[0].returnTarget) {
                DPRINTF(FTBuRAS, "pc %llx pred to be ret but s1 target %llx, s2 target %llx\n", predsOfEachStage[1].bbStart, predsOfEachStage[0].returnTarget, predsOfEachStage[1].returnTarget);
            }
        }

        printFullFTBPrediction(*chosen);
        dbpFtbStats.predsOfEachStage[first_hit_stage]++;
    } else {
        bubblesToCreate = 0;
        receivedPred = true;
        DPRINTF(LoopBuffer, "Do not generate final pred when loop buffer is active\n");
        DPRINTF(DecoupleBP, "Do not generate final pred when loop buffer is active\n");
    }

    DPRINTF(Override, "Ends generateFinalPredAndCreateBubbles(), numOverrideBubbles is %d,"
                      "receivedPred is set true.\n", bubblesToCreate);

    generateAndSetNewFetchStream();

    return bubblesToCreate;
}

bool
DecoupledBPUWithFTB::trySupplyFetchWithTarget(Addr fetch_demand_pc, bool &fetch_target_in_loop)
{
    return fetchTargetQueue.trySupplyFetchWithTarget(fetch_demand_pc, fetch_target_in_loop);
}

std::pair<bool, bool>
DecoupledBPUWithFTB::decoupledPredict(const StaticInstPtr &inst,
                               const InstSeqNum &seqNum, PCStateBase &pc,
                               ThreadID tid, unsigned &currentLoopIter)
{
    std::unique_ptr<PCStateBase> target(pc.clone());

    DPRINTF(DecoupleBP, "looking up pc %#lx\n", pc.instAddr());
    auto target_avail = fetchTargetQueue.fetchTargetAvailable();

    DPRINTF(DecoupleBP, "Supplying fetch with target ID %lu\n",
            fetchTargetQueue.getSupplyingTargetId());

    if (!target_avail) {
        DPRINTF(DecoupleBP,
                "No ftq entry to fetch, return dummy prediction\n");
        // todo pass these with reference
        // TODO: do we need to update PC if not taken?
        return std::make_pair(false, true);
    }
    currentFtqEntryInstNum++;

    const auto &target_to_fetch = fetchTargetQueue.getTarget();

    // found corresponding entry
    auto start = target_to_fetch.startPC;
    auto end = target_to_fetch.endPC;
    auto taken_pc = target_to_fetch.takenPC;
    auto in_loop = target_to_fetch.inLoop;
    auto loop_iter = target_to_fetch.iter;
    auto loop_exit = target_to_fetch.isExit;
    DPRINTF(DecoupleBP, "Responsing fetch with");
    printFetchTarget(target_to_fetch, "");

    auto current_loop_iter = fetchTargetQueue.getCurrentLoopIter();
    currentLoopIter = current_loop_iter;

    // supplying ftq entry might be taken before pc
    // because it might just be updated last cycle
    // but last cycle ftq tells fetch that this is a miss stream
    assert(pc.instAddr() < end && pc.instAddr() >= start);
    bool raw_taken = pc.instAddr() == taken_pc && target_to_fetch.taken;
    bool taken = raw_taken;
    bool run_out_of_this_entry = false;
    // an ftq entry may consists of multiple loop iterations,
    // so we need to check if we are at the end of this loop iteration,
    // since taken and not taken can both exist in the same ftq entry
    if (in_loop) {
        DPRINTF(LoopBuffer, "current loop iter %d, loop_iter %d, loop_exit %d\n",
            current_loop_iter, loop_iter, loop_exit);
        if (raw_taken) {
            if (current_loop_iter >= loop_iter - 1) {
                run_out_of_this_entry = true;
                if (loop_exit) {
                    taken = false;
                    lb.tryUnpin();
                    DPRINTF(LoopBuffer, "modifying taken to false because of loop exit\n");
                }
            }
            fetchTargetQueue.incCurrentLoopIter(loop_iter);
        }
    } else {
        if (taken) {
            run_out_of_this_entry = true;
        }
    }

    if (taken) {
        auto &rtarget = target->as<GenericISA::PCStateWithNext>();
        rtarget.pc(target_to_fetch.target);
        // TODO: how about compressed?
        rtarget.npc(target_to_fetch.target + 4);
        rtarget.uReset();
        DPRINTF(DecoupleBP,
                "Predicted pc: %#lx, upc: %#lx, npc(meaningless): %#lx, instSeqNum: %d\n",
                target->instAddr(), rtarget.upc(), rtarget.npc(), seqNum);
        set(pc, *target);
    } else {
        inst->advancePC(*target);
        if (target->instAddr() >= end) {
            run_out_of_this_entry = true;
        }
    }
        DPRINTF(DecoupleBP, "Predict it %staken to %#lx\n", taken ? "" : "not ",
                target->instAddr());

    if (run_out_of_this_entry) {
        // dequeue the entry
        const auto fsqId = target_to_fetch.fsqID;
        DPRINTF(DecoupleBP, "running out of ftq entry %lu with %d insts\n",
                fetchTargetQueue.getSupplyingTargetId(), currentFtqEntryInstNum);
        fetchTargetQueue.finishCurrentFetchTarget();
        // record inst fetched in fsq entry
        auto it = fetchStreamQueue.find(fsqId);
        assert(it != fetchStreamQueue.end());
        it->second.fetchInstNum = currentFtqEntryInstNum;
        currentFtqEntryInstNum = 0;
    }

    return std::make_pair(taken, run_out_of_this_entry);
}

void
DecoupledBPUWithFTB::controlSquash(unsigned target_id, unsigned stream_id,
                            const PCStateBase &control_pc,
                            const PCStateBase &corr_target,
                            const StaticInstPtr &static_inst,
                            unsigned control_inst_size, bool actually_taken,
                            const InstSeqNum &seq, ThreadID tid,
                            const unsigned &currentLoopIter, const bool fromCommit)
{
    dbpFtbStats.controlSquash++;

    bool is_conditional = static_inst->isCondCtrl();
    bool is_indirect = static_inst->isIndirectCtrl();
    // bool is_call = static_inst->isCall() && !static_inst->isNonSpeculative();
    // bool is_return = static_inst->isReturn() && !static_inst->isNonSpeculative();




    squashing = true;

    // check sanity
    auto squashing_stream_it = fetchStreamQueue.find(stream_id);

    if (squashing_stream_it == fetchStreamQueue.end()) {
        assert(!fetchStreamQueue.empty());
        // assert(fetchStreamQueue.rbegin()->second.getNextStreamStart() == MaxAddr);
        DPRINTF(
            DecoupleBP || debugFlagOn,
            "The squashing stream is insane, ignore squash on it");
        return;
    }

    // get corresponding stream entry
    auto &stream = squashing_stream_it->second;
    // get target from ras preserved info for decode-detected unpredicted returns
    Addr real_target = corr_target.instAddr();
    if (!fromCommit && static_inst->isReturn() && !static_inst->isNonSpeculative()) {
        // get ret addr from ras meta
        real_target = ras->getTopAddrFromMetas(stream);
        // TODO: set real target to dynamic inst
    }


    // recover pc
    s0PC = real_target;


    auto squashSrc = SQUASH_SRC_COMMIT;
    auto squashBranchInfo = BranchInfo(control_pc.instAddr(), real_target, static_inst, control_inst_size);
    auto predCycle = stream.predCycle;
    auto squashLat = curCycle() - predCycle;
    if (!fromCommit) {
        if (stream.isHit) {
            stream.falseHit = true;
        }
        squashSrc = SQUASH_SRC_DECODE;
        stream.squashSource = squashSrc;
        dbpFtbStats.decodeControlSquashLatencyDist.sample(squashLat, 1);

        // per branch type
        if (squashBranchInfo.isCond) {
            dbpFtbStats.controlDecodeSquashOfCond++;
        } else {
            dbpFtbStats.controlDecodeSquashOfUncond++;
            if (!squashBranchInfo.isIndirect) {
                dbpFtbStats.controlDecodeSquashOfUncondDirect++;
            } else {
                dbpFtbStats.controlDecodeSquashOfUncondIndirect++;
                if (squashBranchInfo.isReturn) {
                    dbpFtbStats.controlDecodeSquashOfUncondReturn++;
                }
            }
        }
    } else {
        dbpFtbStats.commitControlSquashLatencyDist.sample(squashLat, 1);
        // per branch type
        if (squashBranchInfo.isCond) {
            dbpFtbStats.controlCommitSquashOfCond++;
        } else {
            dbpFtbStats.controlCommitSquashOfUncond++;
            if (!squashBranchInfo.isIndirect) {
                dbpFtbStats.controlCommitSquashOfUncondDirect++;
            } else {
                dbpFtbStats.controlCommitSquashOfUncondIndirect++;
                if (squashBranchInfo.isReturn) {
                    dbpFtbStats.controlCommitSquashOfUncondReturn++;
                }
            }
        }
    }
    stream.squashInfos[control_pc.instAddr()] = std::make_tuple(SQUASH_CTRL, squashSrc, squashBranchInfo);

    if (stream.isExit) {
        dbpFtbStats.controlSquashOnLoopPredictorPredExit++;
    }
    if (stream.fromLoopBuffer) {
        dbpFtbStats.squashOnLoopBufferPredBlock++;
        if (stream.isDouble) {
            dbpFtbStats.squashOnLoopBufferDoublePredBlock++;
        }
    }

    auto pc = stream.startPC;
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (pc == ObservingPC) {
        debugFlagOn = true;
    }
    if (control_pc.instAddr() == ObservingPC || control_pc.instAddr() == ObservingPC2) {
        debugFlagOn = true;
    }

    DPRINTF(DecoupleBPHist,
            "stream start=%#lx, predict on hist: %s\n", stream.startPC,
            stream.history);

    DPRINTF(DecoupleBP || debugFlagOn,
            "Control squash: ftq_id=%lu, fsq_id=%lu,"
            " control_pc=%#lx, real_target=%#lx, is_conditional=%u, "
            "is_indirect=%u, actually_taken=%u, branch seq: %lu\n",
            target_id, stream_id, control_pc.instAddr(),
            real_target, is_conditional, is_indirect,
            actually_taken, seq);

    dumpFsq("Before control squash");

    // streamLoopPredictor->restoreLoopTable(stream.mruLoop);
    // streamLoopPredictor->controlSquash(stream_id, stream, control_pc.instAddr(), corr_target.instAddr());

    stream.squashType = SQUASH_CTRL;

    if (enableJumpAheadPredictor && stream.jaHit) {
        jap.invalidate(stream.startPC);
        dbpFtbStats.controlSquashOnJaHitBlocks++;
    }

    FetchTargetId ftq_demand_stream_id;


    stream.exeBranchInfo = squashBranchInfo;
    stream.exeTaken = actually_taken;
    stream.squashPC = control_pc.instAddr();

    if (enableLoopPredictor) {
        lp.startRepair();
        // recover loop predictor
        // we should check if the numBr possible loop branches should be recovered
        for (int i = 0; i < numBr; ++i) {
            // loop branches behind the squashed branch should be recovered
            if (stream.loopRedirectInfos[i].e.valid && control_pc.instAddr() <= stream.loopRedirectInfos[i].branch_pc) {
                DPRINTF(DecoupleBP, "Recover loop predictor for %#lx\n", stream.loopRedirectInfos[i].branch_pc);
                lp.recover(stream.loopRedirectInfos[i], actually_taken, control_pc.instAddr(), true, false, currentLoopIter);
            }
        }
        for (auto &info : stream.unseenLoopRedirectInfos) {
            if (info.e.valid && control_pc.instAddr() <= info.branch_pc) {
                DPRINTF(DecoupleBP, "Recover loop predictor for unseen branch %#lx\n", info.branch_pc);
                lp.recover(info, actually_taken, control_pc.instAddr(), true, false, currentLoopIter);
            }
        }
    }

    squashStreamAfter(stream_id);

    if (enableLoopPredictor) {
        lp.endRepair();
    }

    if (enableLoopBuffer) {
        lb.clearState();
    }

    stream.resolved = true;

    // recover history to the moment doing prediction
    DPRINTF(DecoupleBPHist,
             "Recover history %s\nto %s\n", s0History, stream.history);
    s0History = stream.history;

    // recover history info
    int real_shamt;
    bool real_taken;
    std::tie(real_shamt, real_taken) = stream.getHistInfoDuringSquash(control_pc.instAddr(), is_conditional, actually_taken, numBr);
    for (int i = 0; i < numComponents; ++i) {
        components[i]->recoverHist(s0History, stream, real_shamt, real_taken);
    }
    histShiftIn(real_shamt, real_taken, s0History);
    historyManager.squash(stream_id, real_shamt, real_taken, stream.exeBranchInfo);
    checkHistory(s0History);
    tage->checkFoldedHist(s0History, "control squash");

    DPRINTF(DecoupleBPHist,
                "Shift in history %s\n", s0History);

    printStream(stream);

    if (enableLoopBuffer) {
        lb.recordNewestStreamOutsideLoop(stream);
    }

    
    // inc stream id because current stream ends
    // now stream always ends
    ftq_demand_stream_id = stream_id + 1;
    fsqId = stream_id + 1;

    dumpFsq("After control squash");

    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id,
                            real_target);

    fetchTargetQueue.dump("After control squash");

    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
            "After squash, FSQ head Id=%lu, demand stream Id=%lu, Fetch "
            "demanded target Id=%lu\n",
            fsqId, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());


}

void
DecoupledBPUWithFTB::nonControlSquash(unsigned target_id, unsigned stream_id,
                               const PCStateBase &inst_pc,
                               const InstSeqNum seq, ThreadID tid, const unsigned &currentLoopIter)
{
    dbpFtbStats.nonControlSquash++;
    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
            "non control squash: target id: %lu, stream id: %lu, inst_pc: %x, "
            "seq: %lu\n",
            target_id, stream_id, inst_pc.instAddr(), seq);
    squashing = true;

    dumpFsq("before non-control squash");

    // make sure the stream is in FSQ
    auto it = fetchStreamQueue.find(stream_id);
    assert(it != fetchStreamQueue.end());

    auto ftq_demand_stream_id = stream_id;
    auto &stream = it->second;
    stream.squashInfos[inst_pc.instAddr()] = std::make_tuple(SQUASH_OTHER, SQUASH_SRC_COMMIT, BranchInfo());
    auto predCycle = stream.predCycle;
    auto squashLat = curCycle() - predCycle;
    dbpFtbStats.commitNonControlSquashLatencyDist.sample(squashLat, 1);

    if (enableLoopPredictor) {
        lp.startRepair();
        // recover loop predictor
        // we should check if the numBr possible loop branches should be recovered
        for (int i = 0; i < numBr; ++i) {
            // loop branches behind the squashed branch should be recovered
            if (stream.loopRedirectInfos[i].e.valid && inst_pc.instAddr() <= stream.loopRedirectInfos[i].branch_pc) {
                DPRINTF(DecoupleBP, "Recover loop predictor for %#lx\n", stream.loopRedirectInfos[i].branch_pc);
                lp.recover(stream.loopRedirectInfos[i], false, inst_pc.instAddr(), false, false, currentLoopIter);
            }
        }
        for (auto &info : stream.unseenLoopRedirectInfos) {
            if (info.e.valid && inst_pc.instAddr() <= info.branch_pc) {
                DPRINTF(DecoupleBP, "Recover loop predictor for unseen branch %#lx\n", info.branch_pc);
                lp.recover(info, false, inst_pc.instAddr(), false, false, currentLoopIter);
            }
        }
    }


    squashStreamAfter(stream_id);

    if (enableLoopPredictor) {
        lp.endRepair();
    }

    if (enableLoopBuffer) {
        lb.clearState();
    }

    
    if (stream.isExit) {
        dbpFtbStats.nonControlSquashOnLoopPredictorPredExit++;
    }
    if (stream.fromLoopBuffer) {
        dbpFtbStats.squashOnLoopBufferPredBlock++;
        if (stream.isDouble) {
            dbpFtbStats.squashOnLoopBufferDoublePredBlock++;
        }
    }

    stream.exeTaken = false;
    stream.resolved = true;
    stream.squashPC = inst_pc.instAddr();
    stream.squashType = SQUASH_OTHER;

    if (enableJumpAheadPredictor && stream.jaHit) {
        dbpFtbStats.nonControlSquashOnJaHitBlocks++;
    }

    // recover history info
    s0History = it->second.history;
    int real_shamt;
    bool real_taken;
    std::tie(real_shamt, real_taken) = stream.getHistInfoDuringSquash(inst_pc.instAddr(), false, false, numBr);
    for (int i = 0; i < numComponents; ++i) {
        components[i]->recoverHist(s0History, stream, real_shamt, real_taken);
    }
    histShiftIn(real_shamt, real_taken, s0History);
    historyManager.squash(stream_id, real_shamt, real_taken, BranchInfo());
    checkHistory(s0History);
    tage->checkFoldedHist(s0History, "non control squash");
    // fetching from a new fsq entry
    auto pc = inst_pc.instAddr();
    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id + 1, pc);

    if (enableLoopBuffer) {
        lb.recordNewestStreamOutsideLoop(stream);
    }

    s0PC = pc;
    fsqId = stream_id + 1;

    if (pc == ObservingPC) dumpFsq("after non-control squash");
    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
            "After squash, FSQ head Id=%lu, s0pc=%#lx, demand stream Id=%lu, "
            "Fetch demanded target Id=%lu\n",
            fsqId, s0PC, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());
}

void
DecoupledBPUWithFTB::trapSquash(unsigned target_id, unsigned stream_id,
                         Addr last_committed_pc, const PCStateBase &inst_pc,
                         ThreadID tid, const unsigned &currentLoopIter)
{
    dbpFtbStats.trapSquash++;
    DPRINTF(DecoupleBP || debugFlagOn,
            "Trap squash: target id: %lu, stream id: %lu, inst_pc: %#lx\n",
            target_id, stream_id, inst_pc.instAddr());
    squashing = true;

    auto pc = inst_pc.instAddr();

    if (pc == ObservingPC) dumpFsq("before trap squash");

    auto it = fetchStreamQueue.find(stream_id);
    assert(it != fetchStreamQueue.end());
    auto &stream = it->second;
    stream.squashInfos[inst_pc.instAddr()] = std::make_tuple(SQUASH_TRAP, SQUASH_SRC_COMMIT, BranchInfo());
    auto predCycle = stream.predCycle;
    auto squashLat = curCycle() - predCycle;
    dbpFtbStats.commitTrapSquashLatencyDist.sample(squashLat, 1);

    if (stream.isExit) {
        dbpFtbStats.trapSquashOnLoopPredictorPredExit++;
    }
    if (stream.fromLoopBuffer) {
        dbpFtbStats.squashOnLoopBufferPredBlock++;
        if (stream.isDouble) {
            dbpFtbStats.squashOnLoopBufferDoublePredBlock++;
        }
    }

    stream.resolved = true;
    stream.exeTaken = false;
    stream.squashPC = inst_pc.instAddr();
    stream.squashType = SQUASH_TRAP;

    if (enableJumpAheadPredictor && stream.jaHit) {
        dbpFtbStats.trapSquashOnJaHitBlocks++;
    }

    if (enableLoopPredictor) {
        // recover loop predictor
        // we should check if the numBr possible loop branches should be recovered
        for (int i = 0; i < numBr; ++i) {
            // loop branches behind the squashed branch should be recovered
            if (stream.loopRedirectInfos[i].e.valid && inst_pc.instAddr() <= stream.loopRedirectInfos[i].branch_pc) {
                DPRINTF(DecoupleBP, "Recover loop predictor for %#lx\n", stream.loopRedirectInfos[i].branch_pc);
                lp.recover(stream.loopRedirectInfos[i], false, inst_pc.instAddr(), false, false, currentLoopIter);
            }
        }
        for (auto &info : stream.unseenLoopRedirectInfos) {
            if (info.e.valid && inst_pc.instAddr() <= info.branch_pc) {
                DPRINTF(DecoupleBP, "Recover loop predictor for unseen branch %#lx\n", info.branch_pc);
                lp.recover(info, false, inst_pc.instAddr(), false, false, currentLoopIter);
            }
        }
    }

    squashStreamAfter(stream_id);

    if (enableLoopPredictor) {
        lp.endRepair();
    }

    if (enableLoopBuffer) {
        lb.clearState();
    }

    // recover history info
    s0History = stream.history;
    int real_shamt;
    bool real_taken;
    std::tie(real_shamt, real_taken) = stream.getHistInfoDuringSquash(inst_pc.instAddr(), false, false, numBr);
    for (int i = 0; i < numComponents; ++i) {
        components[i]->recoverHist(s0History, stream, real_shamt, real_taken);
    }
    histShiftIn(real_shamt, real_taken, s0History);
    historyManager.squash(stream_id, real_shamt, real_taken, BranchInfo());
    checkHistory(s0History);
    tage->checkFoldedHist(s0History, "trap squash");

    // inc stream id because current stream is disturbed
    auto ftq_demand_stream_id = stream_id + 1;
    fsqId = stream_id + 1;

    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id,
                            inst_pc.instAddr());
    
    if (enableLoopBuffer) {
        lb.recordNewestStreamOutsideLoop(stream);
    }

    s0PC = inst_pc.instAddr();

    DPRINTF(DecoupleBP,
            "After trap squash, FSQ head Id=%lu, s0pc=%#lx, demand stream "
            "Id=%lu, Fetch demanded target Id=%lu\n",
            fsqId, s0PC, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());
}

void DecoupledBPUWithFTB::update(unsigned stream_id, ThreadID tid)
{
    // aka, commit stream
    // commit controls in local prediction history buffer to committedSeq
    // mark all committed control instructions as correct
    // do not need to dequeue when empty
    if (fetchStreamQueue.empty())
        return;
    auto it = fetchStreamQueue.begin();
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    while (it != fetchStreamQueue.end() && stream_id >= it->first) {
        auto &stream = it->second;
        // dequeue
        DPRINTF(DecoupleBP, "dequeueing stream id: %lu, entry below:\n",
                it->first);
        bool miss_predicted = stream.squashType == SQUASH_CTRL;
        if (miss_predicted) {
            DPRINTF(FTBITTAGE || (stream.squashPC == 0x1e0eb6), "miss predicted stream.startAddr=%#lx\n", stream.startPC);
        }
        if (miss_predicted && stream.exeBranchInfo.isIndirect) {
            topMispredIndirect[stream.startPC]++;
        }
        // if (stream.startPC == ObservingPC) {
        //     debugFlagOn = true;
        // }
        // if (stream.exeBranchPC == ObservingPC2) {
        //     debugFlagOn = true;
        // }
        DPRINTF(DecoupleBP || debugFlagOn,
                "Commit stream start %#lx, which is %s predicted, "
                "final br addr: %#lx, final target: %#lx, pred br addr: %#lx, "
                "pred target: %#lx\n",
                stream.startPC, miss_predicted ? "miss" : "correctly",
                stream.exeBranchInfo.pc, stream.exeBranchInfo.target,
                stream.predBranchInfo.pc, stream.predBranchInfo.target);
        
        if (stream.isHit && !stream.falseHit) {
            dbpFtbStats.ftbHit++;
        } else {
            if (stream.exeTaken) {
                dbpFtbStats.ftbMiss++;
                DPRINTF(FTB, "FTB miss detected when update, stream start %#lx, predTick %lu, printing branch info:\n", stream.startPC, stream.predTick);
                auto &slot = stream.exeBranchInfo;
                DPRINTF(FTB, "    pc:%#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d\n",
                slot.pc, slot.size, slot.target, slot.isCond, slot.isIndirect, slot.isCall, slot.isReturn);
                // try to find branch indentified by exeBranchInfo in commitMispredictions
                const auto misp_it = stream.commitMispredictions.find(slot.pc);
                bool found = misp_it != stream.commitMispredictions.end();
                DPRINTF(DBPFTBStats, "fsqID: %d, inst causing ftb miss: %#lx, found in commitMispredictions: %d\n", it->first, slot.pc, found);
                if (!found) {
                    dbpFtbStats.ftbMissInstNotCommitted++;
                    DPRINTF(DBPFTBStats, "fsqID: %d, pc: %#lx not found in commitMispredictions\n", it->first, slot.pc);
                    DPRINTF(DBPFTBStats, "dumping commitMispredictions:\n");
                    for (auto &pair : stream.commitMispredictions) {
                        DPRINTF(DBPFTBStats, "    pc: %#lx, mispred: %d\n", pair.first, pair.second);
                    }
                    if (stream.commitMispredictions.empty()) {
                        dbpFtbStats.ftbMissWithNoBranches++;
                    } else {
                        bool someBranchesMispredicted = false;
                        for (auto pc_misp_pair : stream.commitMispredictions) {
                            someBranchesMispredicted |= pc_misp_pair.second;
                        }
                        if (!someBranchesMispredicted) {
                            dbpFtbStats.ftbMissWithNoMispreds++;
                        }
                    }
                } else {
                    bool miss = misp_it->second;
                    if (!miss) {
                        dbpFtbStats.ftbMissInstNotMispredicted++;
                        DPRINTF(DBPFTBStats, "fsqID: %d, pc: %#lx found in commitMispredictions, but not mispredicted\n", it->first, slot.pc);
                    } else {
                        dbpFtbStats.ftbMissInstMispredicted++;
                    }
                }
            } else {
                dbpFtbStats.ftbMissWithNoHarm++;
            }
            if (stream.falseHit) {
                dbpFtbStats.commitFalseHit++;
            }
        }
        dbpFtbStats.commitPredsFromEachStage[stream.predSource]++;

        if (stream.squashType != SQUASH_NONE) {
            dbpFtbStats.committedStreamHadReceivedSquash++;
            if (stream.squashSource == SQUASH_SRC_DECODE) {
                dbpFtbStats.committedStreamSquashFromDecode++;
            } else if (stream.squashSource == SQUASH_SRC_COMMIT ) {
                dbpFtbStats.committedStreamSquashFromCommit++;
            }
        }

        for (auto &kv : stream.squashInfos) {
            auto &pc = kv.first;
            auto &squash_tuple = kv.second;
            auto &squashType = std::get<0>(squash_tuple);
            auto &squashSrc = std::get<1>(squash_tuple);
            auto &squashBranchInfo = std::get<2>(squash_tuple);
            DPRINTF(DBPFTBStats, "fsqID %d, squash info: pc=%#lx, squashType=%d, squashSrc=%d\n", it->first, pc, squashType, squashSrc);
            // make sure the instruction triggering squash is actually committed
            const auto mis_it = stream.commitMispredictions.find(pc);
            if (mis_it != stream.commitMispredictions.end()) {
                // previously control squashed
                if (squashType == SQUASH_CTRL) {
                    dbpFtbStats.controlSquashedCommitted++;
                    // this squashed inst not mispredicted?
                    if (!mis_it->second) {
                        if (stream.exeBranchInfo.isCond) {
                            dbpFtbStats.committedSquashedCondNotMispredicted++;
                            if (squashSrc == SQUASH_SRC_DECODE) {
                                dbpFtbStats.committedDecodeSquashedCondNotMispredicted++;
                            } else if (squashSrc == SQUASH_SRC_COMMIT) {
                                dbpFtbStats.committedCommitSquashedCondNotMispredicted++;
                            }
                        } else if (stream.exeBranchInfo.isUncond()) {
                            dbpFtbStats.committedSquashedUncondNotMispredicted++;
                            if (squashSrc == SQUASH_SRC_DECODE) {
                                dbpFtbStats.committedDecodeSquashedUncondNotMispredicted++;
                            } else if (squashSrc == SQUASH_SRC_COMMIT) {
                                dbpFtbStats.committedCommitSquashedUncondNotMispredicted++;
                            }
                        }
                    } else {
                        dbpFtbStats.committedSquashedBranchFinallyMispredicted++;
                    }

                    // per branch type
                    if (squashSrc == SQUASH_SRC_DECODE) {
                        if (squashBranchInfo.isCond) {
                            dbpFtbStats.controlDecodeSquashedCondCommitted++;
                        }
                        if (squashBranchInfo.isUncond()) {
                            dbpFtbStats.controlDecodeSquashedUncondCommitted++;
                            if (!squashBranchInfo.isIndirect) {
                                dbpFtbStats.controlDecodeSquashedUncondDirectCommitted++;
                            } else {
                                dbpFtbStats.controlDecodeSquashedUncondIndirectCommitted++;
                                if (squashBranchInfo.isReturn) {
                                    dbpFtbStats.controlDecodeSquashedUncondReturnCommitted++;
                                }
                            }
                        }
                    } else if (squashSrc == SQUASH_SRC_COMMIT) {
                        if (squashBranchInfo.isCond) {
                            dbpFtbStats.controlCommitSquashedCondCommitted++;
                        }
                        if (squashBranchInfo.isUncond()) {
                            dbpFtbStats.controlCommitSquashedUncondCommitted++;
                            if (!squashBranchInfo.isIndirect) {
                                dbpFtbStats.controlCommitSquashedUncondDirectCommitted++;
                            } else {
                                dbpFtbStats.controlCommitSquashedUncondIndirectCommitted++;
                                if (squashBranchInfo.isReturn) {
                                    dbpFtbStats.controlCommitSquashedUncondReturnCommitted++;
                                }
                            }
                        }
                    }

                }
            } else {
                // not committed, it means this instruction is on a wrong path
                if (squashType == SQUASH_CTRL) {
                    dbpFtbStats.controlSquashedNotCommitted++;
                    // per branch type
                    if (squashSrc == SQUASH_SRC_DECODE) {
                        if (squashBranchInfo.isCond) {
                            dbpFtbStats.controlDecodeSquashedCondNotCommitted++;
                        }
                        if (squashBranchInfo.isUncond()) {
                            dbpFtbStats.controlDecodeSquashedUncondNotCommitted++;
                            if (!squashBranchInfo.isIndirect) {
                                dbpFtbStats.controlDecodeSquashedUncondDirectNotCommitted++;
                            } else {
                                dbpFtbStats.controlDecodeSquashedUncondIndirectNotCommitted++;
                                if (squashBranchInfo.isReturn) {
                                    dbpFtbStats.controlDecodeSquashedUncondReturnNotCommitted++;
                                }
                            }
                        }
                    } else if (squashSrc == SQUASH_SRC_COMMIT) {
                        if (squashBranchInfo.isCond) {
                            dbpFtbStats.controlCommitSquashedCondNotCommitted++;
                        }
                        if (squashBranchInfo.isUncond()) {
                            dbpFtbStats.controlCommitSquashedUncondNotCommitted++;
                            if (!squashBranchInfo.isIndirect) {
                                dbpFtbStats.controlCommitSquashedUncondDirectNotCommitted++;
                            } else {
                                dbpFtbStats.controlCommitSquashedUncondIndirectNotCommitted++;
                                if (squashBranchInfo.isReturn) {
                                    dbpFtbStats.controlCommitSquashedUncondReturnNotCommitted++;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (stream.isHit || stream.exeTaken) {
            // update latency stats
            auto updateLat = curCycle() - stream.predCycle;
            dbpFtbStats.updateLatencyDist.sample(updateLat, 1);

            // generate new ftb entry first
            // each component will use info of this entry to update
            ftb->getAndSetNewFTBEntry(stream);
            for (int i = 0; i < numComponents; ++i) {
                components[i]->update(stream);
            }
            // ftb entry stats
            auto it = totalFTBEntries.find(stream.startPC);
            if (it == totalFTBEntries.end()) {
                auto &ftb_entry = stream.updateFTBEntry;
                totalFTBEntries[stream.startPC] = std::make_pair(ftb_entry, 1);
                dbpFtbStats.ftbEntriesWithDifferentStart++;
                if (ftb_entry.slots.size() == 1) {
                    if (ftb_entry.slots[0].pc == stream.startPC && ftb_entry.slots[0].isUncond()) {
                        dbpFtbStats.ftbEntriesWithOnlyOneJump++;
                    }
                }
            } else {
                it->second.second++;
                it->second.first = stream.updateFTBEntry;
            }
        }

        if (enableJumpAheadPredictor) {
            if (stream.isHit || stream.exeTaken || stream.squashType != SQUASH_NONE) {
                // this block predicted, if we already recorded enough non-pred blocks,
                // then we should write into storage
                // update predicted blocks of ja info
                jap.tryUpdate(jaInfo, stream.startPC);
                jaInfo.setPredictedBlock(stream.startPC, stream.updateFTBEntry);
            } else {
                // this block has no pred, increment non-pred count
                jaInfo.incrementNoPredBlockCount(stream.startPC);
            }

            // do some statistics
            if (stream.jaHit) {
                int skippedBlocks = stream.jaEntry.jumpAheadBlockNum - 1;
                dbpFtbStats.commitJATotalSkippedBlocks += skippedBlocks;
                dbpFtbStats.commitJASkippedBlockNum.sample(skippedBlocks, 1);
                switch (stream.squashType) {
                    case SQUASH_CTRL:
                        dbpFtbStats.commitControlSquashedOnJaHitBlocks++;
                        break;
                    case SQUASH_OTHER:
                        dbpFtbStats.commitNonControlSquashedOnJaHitBlocks++;
                        break;
                    case SQUASH_TRAP:
                        dbpFtbStats.commitTrapSquashedOnJaHitBlocks++;
                        break;
                    default:
                        break;
                }
            }
        }


        // check loop predictor prediction
        auto lp_infos = stream.loopRedirectInfos;
        DPRINTF(LoopPredictor, "at commit fsqid %d, real_branch_pc %#lx, squash type %d, loop predcition infos:\n", it->first, stream.exeBranchInfo.pc, stream.squashType);
        DPRINTF(LoopBuffer, "from loop buffer %d, doubling %d, exit %d\n", stream.fromLoopBuffer, stream.isDouble, stream.isExit);
        for (int i = 0; i < numBr; ++i) {
            auto &lp_info = lp_infos[i];
            DPRINTF(LoopPredictor, "    branch_pc %#lx, end_loop %d, specCnt %d, tripCnt %d, conf %d\n",
                lp_info.branch_pc, lp_info.end_loop, lp_info.e.specCnt, lp_info.e.tripCnt, lp_info.e.conf);
            if (stream.fixNotExits[i]) {
                dbpFtbStats.commitLoopPredictorConfFixNotExit++;
                if (stream.squashType == SQUASH_CTRL && stream.squashPC == lp_info.branch_pc) {
                    dbpFtbStats.commitLoopPredictorConfFixNotExitWrong++;
                }
                if (stream.squashType != SQUASH_CTRL ||
                    (stream.squashType == SQUASH_CTRL && stream.squashPC != lp_info.branch_pc))
                {
                    dbpFtbStats.commitLoopPredictorConfFixNotExitCorrect++;
                }
            }
        }
        if (stream.isExit) {
            dbpFtbStats.commitLoopPredictorExit++;
            if (stream.squashType == SQUASH_NONE) {
                dbpFtbStats.commitLoopPredictorExitCorrect++;
            } else if (stream.squashType == SQUASH_CTRL) {
                // FIXME: distinguish between squash of other branches
                dbpFtbStats.commitLoopPredictorExitWrong++;
            }
        }
        if (stream.fromLoopBuffer) {
            dbpFtbStats.commitBlockInLoopBuffer++;
            if (stream.isDouble) {
                dbpFtbStats.commitDoubleBlockInLoopBuffer++;
            }
            if (stream.squashType != SQUASH_NONE) {
                dbpFtbStats.commitBlockInLoopBufferSquashed++;
                if (stream.isDouble) {
                    dbpFtbStats.commitDoubleBlockInLoopBufferSquashed++;
                }
            }
            auto instNum = lb.getLoopInstNum(stream.startPC);
            if (instNum > 0) {
                dbpFtbStats.commitLoopBufferEntryInstNum.sample(instNum, 1);
                if (stream.isDouble) {
                    dbpFtbStats.commitLoopBufferDoubleEntryInstNum.sample(instNum, 1);
                }
            }
        }
        dbpFtbStats.commitFsqEntryHasInsts.sample(stream.commitInstNum, 1);
        if (stream.commitInstNum >= 0 && stream.commitInstNum <= 16) {
            commitFsqEntryHasInstsVector[stream.commitInstNum]++;
            if (stream.commitInstNum == 1 && stream.exeBranchInfo.isUncond()) {
                dbpFtbStats.commitFsqEntryOnlyHasOneJump++;
            }
        }
        dbpFtbStats.commitFsqEntryFetchedInsts.sample(stream.fetchInstNum, 1);
        if (stream.fetchInstNum >= 0 && stream.fetchInstNum <= 16) {
            commitFsqEntryFetchedInstsVector[stream.fetchInstNum]++;
        }


        if (stream.squashType == SQUASH_CTRL) {
            auto find_it = topMispredicts.find(std::make_pair(stream.startPC, stream.exeBranchInfo.pc));
            if (find_it == topMispredicts.end()) {
                topMispredicts[std::make_pair(stream.startPC, stream.exeBranchInfo.pc)] = 1;
            } else {
                find_it->second++;
            }

            // if (stream.isMiss /* && stream.exeBranchPC == ObservingPC */) {
            //     missCount++;
            // }

            // if (stream.exeBranchPC == ObservingPC) {
            //     debugFlagOn = true;
            //     auto misTripCount = misPredTripCount.find(stream.tripCount);
            //     if (misTripCount == misPredTripCount.end()) {
            //         misPredTripCount[stream.tripCount] = 1;
            //     } else {
            //         misPredTripCount[stream.tripCount]++;
            //     }
            //     DPRINTF(DecoupleBP || debugFlagOn, "commit mispredicted stream %lu\n", it->first);
            // }
        }

        if (/* stream.startPC == ObservingPC &&  */stream.squashType == SQUASH_CTRL) {
            auto hist(stream.history);
            hist.resize(18);
            uint64_t pattern = hist.to_ulong();
            auto find_it = topMispredHist.find(pattern);
            if (find_it == topMispredHist.end()) {
                topMispredHist[pattern] = 1;
            } else {
                find_it->second++;
            }
        }



        if (enableLoopBuffer) {
            // if current stream is a short loop, try to peek loop buffer
            if (stream.startPC == lastCommittedStream.startPC &&
                lastCommittedStream.exeTaken && stream.exeTaken &&
                lastCommittedStream.exeBranchInfo.target == stream.exeBranchInfo.target &&
                lastCommittedStream.exeBranchInfo.pc == stream.exeBranchInfo.pc &&
                stream.exeBranchInfo.target == stream.startPC) {

                DPRINTF(DecoupleBP, "stream %lu is a loop, lastCommittedStream:\n", it->first);
                printStream(lastCommittedStream);
                DPRINTF(LoopBuffer, "commit peek loop buffer\n");
                lb.commitLoopPeek(stream.startPC, lastCommittedStream.exeBranchInfo.pc);
            }
            lastCommittedStream = stream;
        }

        it = fetchStreamQueue.erase(it);

        dbpFtbStats.fsqEntryCommitted++;
    }
    DPRINTF(DecoupleBP, "after commit stream, fetchStreamQueue size: %lu\n",
            fetchStreamQueue.size());
    if (it != fetchStreamQueue.end()) {
        printStream(it->second);
    }

    historyManager.commit(stream_id);
}

void
DecoupledBPUWithFTB::commitBranch(const DynInstPtr &inst, bool miss)
{
    // do overall statistics
    if (inst->isUncondCtrl()) {
        addCfi(branch_prediction::ftb_pred::DecoupledBPUWithFTB::CfiType::UNCOND, miss);
    }
    if (inst->isCondCtrl()) {
        addCfi(branch_prediction::ftb_pred::DecoupledBPUWithFTB::CfiType::COND, miss);
    }
    if (inst->isReturn()) {
        addCfi(branch_prediction::ftb_pred::DecoupledBPUWithFTB::CfiType::RETURN, miss);
    } else if (inst->isIndirectCtrl()) {
        addCfi(branch_prediction::ftb_pred::DecoupledBPUWithFTB::CfiType::OTHER, miss);
    }
    DPRINTF(DBPFTBStats, "inst=%s\n", inst->staticInst->disassemble(inst->pcState().instAddr()));
    DPRINTF(DBPFTBStats, "isUncondCtrl=%d, isCondCtrl=%d, isReturn=%d, isIndirectCtrl=%d\n",
            inst->isUncondCtrl(), inst->isCondCtrl(), inst->isReturn(), inst->isIndirectCtrl());

    // break down into each predictor and each stage
    // find corresponding fsq entry first
    auto it = fetchStreamQueue.find(inst->fsqId);
    assert(it != fetchStreamQueue.end());

    auto &entry = it->second;
    if (enableBranchTrace) {

        bptrace->write_record(BpTrace(entry, inst, miss));
    }
    Addr branchAddr = inst->pcState().instAddr();
    const auto &rv_pc = inst->pcState().as<RiscvISA::PCState>();
    Addr targetAddr = rv_pc.npc();
    Addr fallThruPC = rv_pc.getFallThruPC();
    BranchInfo info(branchAddr, targetAddr, inst->staticInst, fallThruPC-branchAddr);
    bool taken = rv_pc.branching();
    taken |= inst->isUncondCtrl();
    auto find_it = topMispredictsByBranch.find(std::make_pair(branchAddr, info.getType()));
    MispredType mtype = FAKE_LAST;
    if (miss) {
        // not taken can only be
        if (!taken) {
            assert(info.isCond);
            mtype = DIR_WRONG;
        } else {
            bool predBranchInFTB = false;
            if (entry.isHit) {
                for (auto &slot : entry.predFTBEntry.slots) {
                    if (slot.pc == branchAddr && slot.getType() == info.getType()) {
                        predBranchInFTB = true;
                    }
                }
            }
            if (!predBranchInFTB) {
                mtype = NO_PRED;
            } else {
                if (entry.predTaken && entry.predBranchInfo.pc == branchAddr) {
                    mtype = TARGET_WRONG;
                } else {
                    // pred stream not taken or taken with other branch
                    mtype = DIR_WRONG;
                }
            }
        }
        DPRINTF(Profiling, "branchAddr %#lx is mispredicted, taken %d, type %d, missType %d\n",
            branchAddr, taken, info.getType(), mtype);
        assert(mtype != FAKE_LAST);
    }
    DPRINTF(Profiling, "lookup topMispredictsByBranch for branchAddr %#lx, type %d\n",
            branchAddr, info.getType());
    if (find_it == topMispredictsByBranch.end()) {
        DPRINTF(Profiling, "not found, insert miss %d\n", miss);
        MispredReasonMap rm;
        for (int i = 0; i < FAKE_LAST; i++) {
            rm[MispredType(i)] = mtype == i ? 1 : 0;
        }
        MispredDesc desc = std::make_pair((int)miss, rm);
        topMispredictsByBranch[std::make_pair(branchAddr, info.getType())] = std::make_pair(desc,1);
        dbpFtbStats.staticBranchNum++;
    } else {
        DPRINTF(Profiling, "found, total %d, miss %d\n", find_it->second.second, find_it->second.first.first);
        find_it->second.second++;
        if (miss) {
            find_it->second.first.first++;
            auto it = find_it->second.first.second.find(mtype);
            assert(it != find_it->second.first.second.end());
            it->second++;
        }
    }
    if (taken) {
        auto itt = takenBranches.find(branchAddr);
        DPRINTF(Profiling, "lookup takenBranches for taken branchAddr %#lx\n", branchAddr);
        if (itt == takenBranches.end()) {
            DPRINTF(Profiling, "not found, insert\n");
            takenBranches[branchAddr] = 1;
            dbpFtbStats.staticBranchNumEverTaken++;
        } else {
            DPRINTF(Profiling, "found, inc count %d to %d\n", itt->second, itt->second+1);
            itt->second++;
        }
        DPRINTF(Profiling, "lookup currentPhaseTakenBranches for taken branchAddr %#lx\n", branchAddr);
        auto ittt = currentPhaseTakenBranches.find(branchAddr);
        if (ittt == currentPhaseTakenBranches.end()) {
            DPRINTF(Profiling, "not found, insert\n");
            currentPhaseTakenBranches[branchAddr] = 1;
        } else {
            DPRINTF(Profiling, "found, inc count %d to %d\n", ittt->second, ittt->second+1);
            ittt->second++;
        }
        DPRINTF(Profiling, "lookup currentSubPhaseTakenBranches for taken branchAddr %#lx\n", branchAddr);
        auto ittts = currentSubPhaseTakenBranches.find(branchAddr);
        if (ittts == currentSubPhaseTakenBranches.end()) {
            DPRINTF(Profiling, "not found, insert\n");
            currentSubPhaseTakenBranches[branchAddr] = 1;
        } else {
            DPRINTF(Profiling, "found, inc count %d to %d\n", ittts->second, ittt->second+1);
            ittts->second++;
        }
    }
    entry.commitMispredictions[branchAddr] = miss;
    DPRINTF(DBPFTBStats, "commit branchAddr %#lx, miss %d, fsqID %d\n", branchAddr, miss, inst->fsqId);

    LoopTrace rec;
    LoopEntry predLoopEntry = LoopEntry();
    for (int i = 0; i < numBr; i++) {
        if (entry.loopRedirectInfos[i].branch_pc == inst->pcState().instAddr()) {
            predLoopEntry = entry.loopRedirectInfos[i].e;
            break;
        }
    }
    if (targetAddr < branchAddr || lp.findLoopBranchInStorage(branchAddr)) {
        lp.commitLoopBranch(branchAddr, targetAddr, fallThruPC, miss, rec);
        if (enableLoopDB) {
            rec.set_outside_lp(branchAddr, targetAddr, miss, predLoopEntry.specCnt, predLoopEntry.tripCnt, predLoopEntry.conf);
            lptrace->write_record(rec);
        }
    }

    for (int i = 0; i < numBr; i++) {
        if (entry.loopRedirectInfos[i].branch_pc == inst->pcState().instAddr()) {
            auto &loopEntry = entry.loopRedirectInfos[i].e;
            if (loopEntry.specCnt == loopEntry.tripCnt ||
                (loopEntry.specCnt == loopEntry.tripCnt - 1 && entry.isDouble))
            {
                if (loopEntry.conf != lp.maxConf) {
                    dbpFtbStats.commitLoopExitLoopPredictorNotConf++;
                }
            } else {
                dbpFtbStats.commitLoopExitLoopPredictorNotPredicted++;
            }
        }
    }
    for (auto &info : entry.unseenLoopRedirectInfos) {
        if (info.branch_pc == inst->pcState().instAddr()) {
            auto &loopEntry = info.e;
            dbpFtbStats.commitFTBUnseenLoopBranchInLp++;
            if (loopEntry.specCnt == loopEntry.tripCnt) {
                dbpFtbStats.commitFTBUnseenLoopBranchExitInLp++;
            }
        }
    }
    for (auto component : components) {
        component->commitBranch(entry, inst);
    }
}

void
DecoupledBPUWithFTB::notifyInstCommit(const DynInstPtr &inst)
{
    auto it = fetchStreamQueue.find(inst->fsqId);
    assert(it != fetchStreamQueue.end());
    it->second.commitInstNum++;
    numInstCommitted++;
    DPRINTF(Profiling, "notifyInstCommit, inst=%s, commitInstNum=%d\n",
            inst->staticInst->disassemble(inst->pcState().instAddr()),
            it->second.commitInstNum);
    if (numInstCommitted % phaseSizeByInst == 0) {
        DPRINTF(Profiling, "numInstCommitted %d\n", numInstCommitted);
        int currentPhaseID = numInstCommitted / phaseSizeByInst;
        // dump current phase only once
        if (phaseIdToDump <= currentPhaseID) {
            DPRINTF(Profiling, "dump phase %d\n", phaseIdToDump);
            // fsq entry inst num distribution
            std::vector<int> currentPhaseFsqEntryNumCommittedInstDist;
            std::vector<int> currentPhaseFsqEntryNumFetchedInstDist;
            currentPhaseFsqEntryNumCommittedInstDist.resize(16+1, 0);
            currentPhaseFsqEntryNumFetchedInstDist.resize(16+1, 0);
            // FIXME: parameterize
            for (int i = 0; i <= 16; i++) {
                currentPhaseFsqEntryNumCommittedInstDist[i] = commitFsqEntryHasInstsVector[i] - lastPhaseFsqEntryNumCommittedInstDist[i];
                lastPhaseFsqEntryNumCommittedInstDist[i] = commitFsqEntryHasInstsVector[i];
                currentPhaseFsqEntryNumFetchedInstDist[i] = commitFsqEntryFetchedInstsVector[i] - lastPhaseFsqEntryNumFetchedInstDist[i];
                lastPhaseFsqEntryNumFetchedInstDist[i] = commitFsqEntryFetchedInstsVector[i];
            }
            fsqEntryNumCommittedInstDistByPhase.push_back(currentPhaseFsqEntryNumCommittedInstDist);
            fsqEntryNumFetchedInstDistByPhase.push_back(currentPhaseFsqEntryNumFetchedInstDist);

            // per phase topMispredicts, can be used to calculate static branch
            MispredMap currentPhaseTopMispredictsByBranch;
            for (auto &it : topMispredictsByBranch) {
                auto miss = it.second.first.first;
                auto missMap = it.second.first.second;
                auto total = it.second.second;
                auto last_it = lastPhaseTopMispredictsByBranch.find(it.first);
                if (last_it != lastPhaseTopMispredictsByBranch.end()) {
                    miss -= last_it->second.first.first;
                    total -= last_it->second.second;
                    for (int i = 0; i < FAKE_LAST; i++) {
                        missMap[MispredType(i)] -= last_it->second.first.second[MispredType(i)];
                    }
                }
                if (total > 0) {
                    currentPhaseTopMispredictsByBranch[it.first] = std::make_pair(std::make_pair(miss, missMap), total);
                }
            }
            lastPhaseTopMispredictsByBranch = topMispredictsByBranch;
            topMispredictsByBranchByPhase.push_back(currentPhaseTopMispredictsByBranch);

            takenBranchesByPhase.push_back(currentPhaseTakenBranches);
            currentPhaseTakenBranches.clear();

            // per phase FTB entries
            std::map<Addr, std::pair<FTBEntry, int>> currentPhaseFTBEntries;
            for (auto &it : totalFTBEntries) {
                auto &entry = it.second.first;
                auto visit_cnt = it.second.second;
                auto last_it = lastPhaseFTBEntries.find(it.first);
                if (last_it != lastPhaseFTBEntries.end()) {
                    visit_cnt -= last_it->second.second;
                }
                // use new entries, what if entry of the same start addr changes?
                if (visit_cnt > 0) {
                    currentPhaseFTBEntries[it.first] = std::make_pair(entry, visit_cnt);
                }
            }
            lastPhaseFTBEntries = totalFTBEntries;
            FTBEntriesByPhase.push_back(currentPhaseFTBEntries);

            phaseIdToDump++;
        }
    }

    if (numInstCommitted % subPhaseSizeByInst()) {
        DPRINTF(Profiling, "numInstCommitted %d\n", numInstCommitted);
        int currentSubPhaseID = numInstCommitted / subPhaseSizeByInst();
        if (subPhaseIdToDump <= currentSubPhaseID) {
            DPRINTF(Profiling, "dump sub phase %d\n", subPhaseIdToDump);
            // per phase topMispredicts, can be used to calculate static branch
            MispredMap currentSubPhaseTopMispredictsByBranch;
            for (auto &it : topMispredictsByBranch) {
                auto miss = it.second.first.first;
                auto missMap = it.second.first.second;
                auto total = it.second.second;
                auto last_it = lastSubPhaseTopMispredictsByBranch.find(it.first);
                if (last_it != lastSubPhaseTopMispredictsByBranch.end()) {
                    miss -= last_it->second.first.first;
                    total -= last_it->second.second;
                    for (int i = 0; i < FAKE_LAST; i++) {
                        missMap[MispredType(i)] -= last_it->second.first.second[MispredType(i)];
                    }
                }
                if (total > 0) {
                    currentSubPhaseTopMispredictsByBranch[it.first] = std::make_pair(std::make_pair(miss, missMap), total);
                }
            }
            lastSubPhaseTopMispredictsByBranch = topMispredictsByBranch;
            topMispredictsByBranchBySubPhase.push_back(currentSubPhaseTopMispredictsByBranch);

            takenBranchesBySubPhase.push_back(currentSubPhaseTakenBranches);
            currentSubPhaseTakenBranches.clear();
            subPhaseIdToDump++;
        }
    }
}

void
DecoupledBPUWithFTB::squashStreamAfter(unsigned squash_stream_id)
{
    auto erase_it = fetchStreamQueue.upper_bound(squash_stream_id);
    while (erase_it != fetchStreamQueue.end()) {
        DPRINTF(DecoupleBP || debugFlagOn || erase_it->second.startPC == ObservingPC,
                "Erasing stream %lu when squashing %lu\n", erase_it->first,
                squash_stream_id);
        printStream(erase_it->second);
        if (enableLoopPredictor) {
            DPRINTF(LoopPredictorVerbose, "recovering loop entry in stream %lu\n", erase_it->first);
            for (int i = 0; i < numBr; i++) {
                auto &loopInfo = erase_it->second.loopRedirectInfos[i];
                DPRINTF(LoopPredictorVerbose, "loop entry %d: pc %#lx, endLoop %d, specCnt %d, tripCnty %d, conf %d\n",
                    i, loopInfo.branch_pc, loopInfo.end_loop, loopInfo.e.specCnt, loopInfo.e.tripCnt, loopInfo.e.conf);
                if (loopInfo.e.valid) {
                    lp.recover(loopInfo, false, 0, false, true, 0);
                }
            }
            int j = 0;
            for (auto &info : erase_it->second.unseenLoopRedirectInfos) {
                DPRINTF(LoopPredictorVerbose, "ftb unseen loop entry %d: pc %#lx, endLoop %d, specCnt %d, tripCnty %d, conf %d\n",
                    j+numBr, info.branch_pc, info.end_loop, info.e.specCnt, info.e.tripCnt, info.e.conf);
                if (info.e.valid) {
                    lp.recover(info, false, 0, false, true, 0);
                }
                j++;
            }
        }
        fetchStreamQueue.erase(erase_it++);
    }
}

void
DecoupledBPUWithFTB::dumpFsq(const char *when)
{
    DPRINTF(DecoupleBPProbe, "dumping fsq entries %s...\n", when);
    for (auto it = fetchStreamQueue.begin(); it != fetchStreamQueue.end();
         it++) {
        DPRINTFR(DecoupleBPProbe, "StreamID %lu, ", it->first);
        printStream(it->second);
    }
}

// this funtion use finalPred to enq fsq(ftq) and update s0PC
void
DecoupledBPUWithFTB::tryEnqFetchStream()
{
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (s0PC == ObservingPC) {
        debugFlagOn = true;
    }
    if (!receivedPred) {
        DPRINTF(DecoupleBP, "No received prediction, cannot enq fsq\n");
        DPRINTF(Override, "In tryEnqFetchStream(), received is false.\n");
        return;
    } else {
        DPRINTF(Override, "In tryEnqFetchStream(), received is true.\n");
    }
    if (s0PC == MaxAddr) {
        DPRINTF(DecoupleBP, "s0PC %#lx is insane, cannot make prediction\n", s0PC);
        return;
    }
    // prediction valid, but not ready to enq because of bubbles
    if (numOverrideBubbles > 0) {
        DPRINTF(DecoupleBP, "Waiting for bubble caused by overriding, bubbles rest: %u\n", numOverrideBubbles);
        DPRINTF(Override, "Waiting for bubble caused by overriding, bubbles rest: %u\n", numOverrideBubbles);
        return;
    }
    assert(!streamQueueFull());

    // enqueue fetch stream
    enqueueFetchStream();

    for (int i = 0; i < numStages; i++) {
        predsOfEachStage[i].valid = false;
    }
    receivedPred = false;
    DPRINTF(Override, "In tryFetchEnqStream(), receivedPred reset to false.\n");
    DPRINTF(DecoupleBP || debugFlagOn, "fsqId=%lu\n", fsqId);
}

void
DecoupledBPUWithFTB::setTakenEntryWithStream(const FetchStream &stream_entry, FtqEntry &ftq_entry)
{
    ftq_entry.taken = true;
    ftq_entry.takenPC = stream_entry.getControlPC();
    ftq_entry.endPC = stream_entry.predEndPC;
    ftq_entry.target = stream_entry.getTakenTarget();
    ftq_entry.inLoop = stream_entry.fromLoopBuffer;
    ftq_entry.iter = stream_entry.isDouble ? 2 : stream_entry.fromLoopBuffer ? 1 : 0;
    ftq_entry.isExit = stream_entry.isExit;
    ftq_entry.loopEndPC = stream_entry.getBranchInfo().getEnd();
}

void
DecoupledBPUWithFTB::setNTEntryWithStream(FtqEntry &ftq_entry, Addr end_pc)
{
    ftq_entry.taken = false;
    ftq_entry.takenPC = 0;
    ftq_entry.target = 0;
    ftq_entry.endPC = end_pc;
    ftq_entry.inLoop = false;
    ftq_entry.iter = 0;
    ftq_entry.isExit = false;
    ftq_entry.loopEndPC = 0;
}

void
DecoupledBPUWithFTB::tryEnqFetchTarget()
{
    DPRINTF(DecoupleBP, "Try to enq fetch target\n");
    if (fetchTargetQueue.full()) {
        DPRINTF(DecoupleBP, "FTQ is full\n");
        return;
    }
    if (fetchStreamQueue.empty()) {
        dbpFtbStats.fsqNotValid++;
        // no stream that have not entered ftq
        DPRINTF(DecoupleBP, "No stream to enter ftq in fetchStreamQueue\n");
        return;
    }
    // ftq can accept new cache lines,
    // try to get cache lines from fetchStreamQueue
    // find current stream with ftqEnqfsqID in fetchStreamQueue
    auto &ftq_enq_state = fetchTargetQueue.getEnqState();
    auto it = fetchStreamQueue.find(ftq_enq_state.streamId);
    if (it == fetchStreamQueue.end()) {
        dbpFtbStats.fsqNotValid++;
        // desired stream not found in fsq
        DPRINTF(DecoupleBP, "FTQ enq desired Stream ID %u is not found\n",
                ftq_enq_state.streamId);
        if (streamQueueFull()) {
            dbpFtbStats.fsqFullFetchHungry++;
        }
        return;
    }

    auto &stream_to_enq = it->second;
    Addr end = stream_to_enq.predEndPC;
    DPRINTF(DecoupleBP, "Serve enq PC: %#lx with stream %lu:\n",
            ftq_enq_state.pc, it->first);
    printStream(stream_to_enq);
    

    // We does let ftq to goes beyond fsq now
    if (ftq_enq_state.pc > end) {
        warn("FTQ enq PC %#lx is beyond fsq end %#lx\n",
         ftq_enq_state.pc, end);
    }
    
    assert(ftq_enq_state.pc <= end || (end < 0x20 && (ftq_enq_state.pc + 0x20 < 0x20)));

    // create a new target entry
    FtqEntry ftq_entry;
    ftq_entry.startPC = ftq_enq_state.pc;
    ftq_entry.fsqID = ftq_enq_state.streamId;

    // set prediction results to ftq entry
    Addr thisFtqEntryShouldEndPC = end;
    bool taken = stream_to_enq.getTaken();
    bool inLoop = stream_to_enq.fromLoopBuffer;
    bool loopExit = stream_to_enq.isExit;
    if (enableJumpAheadPredictor) {
        bool jaHit = stream_to_enq.jaHit;
        if (jaHit) {
            int &currentSentBlock = stream_to_enq.currentSentBlock;
            thisFtqEntryShouldEndPC = stream_to_enq.startPC + (currentSentBlock + 1) * 0x20;
            currentSentBlock++;
        }
    }
    Addr loopEndPC = stream_to_enq.getBranchInfo().getEnd();
    if (taken) {
        setTakenEntryWithStream(stream_to_enq, ftq_entry);
    } else {
        setNTEntryWithStream(ftq_entry, thisFtqEntryShouldEndPC);
    }

    // update ftq_enq_state
    // if in loop, next pc will either be loop exit or loop start
    ftq_enq_state.pc = inLoop ?
        loopExit ? loopEndPC : stream_to_enq.getBranchInfo().target :
        taken ? stream_to_enq.getBranchInfo().target : thisFtqEntryShouldEndPC;
    
    // we should not increment streamId to enqueue when ja blocks are not fully consumed
    if (!(enableJumpAheadPredictor && stream_to_enq.jaHit && stream_to_enq.jaHit &&
            stream_to_enq.currentSentBlock < stream_to_enq.jaEntry.jumpAheadBlockNum)) {
        ftq_enq_state.streamId++;
    }
    DPRINTF(DecoupleBP,
            "Update ftqEnqPC to %#lx, FTQ demand stream ID to %lu\n",
            ftq_enq_state.pc, ftq_enq_state.streamId);

    fetchTargetQueue.enqueue(ftq_entry);

    assert(ftq_enq_state.streamId <= fsqId + 1);

    // DPRINTF(DecoupleBP, "a%s stream, next enqueue target: %lu\n",
    //         stream_to_enq.getEnded() ? "n ended" : " miss", ftq_enq_state.nextEnqTargetId);
    printFetchTarget(ftq_entry, "Insert to FTQ");
    fetchTargetQueue.dump("After insert new entry");
}

void
DecoupledBPUWithFTB::histShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history)
{
    if (shamt == 0) {
        return;
    }
    history <<= shamt;
    history[0] = taken;
}

void
DecoupledBPUWithFTB::makeLoopPredictions(FetchStream &entry, bool &endLoop, bool &isDouble, bool &loopConf,
    std::vector<LoopRedirectInfo> &lpRedirectInfos, std::vector<bool> &fixNotExits,
    std::vector<LoopRedirectInfo> &unseenLpRedirectInfos, bool &taken)
{
    // query loop predictor and modify taken result
    // TODO: What if loop branch is predicted not taken?
    // Ans: assume it is loop exit indeed and 
    //      use it to sychronize loop specCnt
    if (finalPred.valid) {
        int i = 0;
        for (auto &slot: finalPred.ftbEntry.slots) {
            if (slot.isCond && (finalPred.getTakenBranchIdx() >= i || finalPred.getTakenBranchIdx() == -1)) {
                assert(finalPred.condTakens.size() > i);
                bool this_cond_pred_taken = finalPred.condTakens[i];
                std::tie(endLoop, lpRedirectInfos[i], isDouble, loopConf) = lp.shouldEndLoop(this_cond_pred_taken, slot.pc, false);
                // for bpu predicted taken branch we need to check
                // whether it is an undetected loop exit
                if (lpRedirectInfos[i].e.valid) {
                    if (loopConf) {
                        // we should only modify the direction of the loop branch, because
                        // a latter branch (outside loop branch) may have other situation
                        finalPred.condTakens[i] = !endLoop;
                        if (endLoop) {
                            DPRINTF(DecoupleBP || debugFlagOn, "Loop predictor says end loop at %#lx\n", slot.pc);
                            dbpFtbStats.predLoopPredictorExit++;
                            entry.isExit = true;
                        } else {
                            if (!this_cond_pred_taken) {
                                dbpFtbStats.predLoopPredictorConfFixNotExit++;
                                fixNotExits[i] = true;
                                DPRINTF(DecoupleBP || debugFlagOn, "Loop predictor says do not end loop at %#lx\n", slot.pc);
                            }
                        }
                        // if (this_cond_pred_taken) {
                        //     if (endLoop) {
                        //         finalPred.condTakens[i] = false;
                        //     }
                        // }
                    } else {
                        if (endLoop) {
                            dbpFtbStats.predLoopPredictorUnconfNotExit++;
                        }
                    }
                }

            }
            i++;
        }
        taken = finalPred.isTaken();
    }
    // check if current prediction block has an unseen loop branch
    Addr end = finalPred.getEnd();
    for (Addr pc = entry.startPC; pc < end; pc += 2) {
        bool inFTB = finalPred.ftbEntry.getSlot(pc).valid;
        if (inFTB) {
            continue;
        }
        LoopRedirectInfo unseenLpInfo;
        std::tie(endLoop, unseenLpInfo, isDouble, loopConf) = lp.shouldEndLoop(false, pc, false);
        if (unseenLpInfo.e.valid) {
            dbpFtbStats.predFTBUnseenLoopBranchInLp++;
            if (endLoop) {
                dbpFtbStats.predFTBUnseenLoopBranchExitInLp++;
            }
            unseenLpRedirectInfos.push_back(unseenLpInfo);
        }
    }
}


// this function enqueues fsq and update s0PC and s0History
// use loop predictor and loop buffer here
void
DecoupledBPUWithFTB::generateAndSetNewFetchStream()
{
    DPRINTF(DecoupleBP, "Try to make new prediction\n");
    FetchStream entry_new;
    auto &entry = entry_new;
    entry.startPC = s0PC;
    entry.predMetas.resize(numComponents);
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (s0PC == ObservingPC) {
        debugFlagOn = true;
    }
    if (finalPred.controlAddr() == ObservingPC || finalPred.controlAddr() == ObservingPC2) {
        debugFlagOn = true;
    }

    // if loop buffer is not activated, use normal prediction from branch predictors
    bool endLoop, isDouble, loopConf;
    std::vector<LoopRedirectInfo> lpRedirectInfos(numBr);
    std::vector<bool> fixNotExits(numBr);
    std::vector<LoopRedirectInfo> unseenLpRedirectInfos;
    if (!enableLoopBuffer || (enableLoopBuffer && !lb.isActive())) {
        entry.fromLoopBuffer = false;
        entry.isDouble = false;
        entry.isExit = false;

        bool taken = finalPred.isTaken();
        bool predReasonable = finalPred.isReasonable();
        if (predReasonable) {
            if (enableLoopPredictor) {
                makeLoopPredictions(entry, endLoop, isDouble, loopConf, lpRedirectInfos,
                    fixNotExits, unseenLpRedirectInfos, taken);
            }
            Addr fallThroughAddr = finalPred.getFallThrough();
            entry.isHit = finalPred.valid;
            entry.falseHit = false;
            entry.predFTBEntry = finalPred.ftbEntry;
            entry.predTaken = taken;
            entry.predEndPC = fallThroughAddr;
            // update s0PC
            Addr nextPC = finalPred.getTarget();
            if (taken) {
                entry.predBranchInfo = finalPred.getTakenSlot().getBranchInfo();
                entry.predBranchInfo.target = nextPC; // use the final target which may be not from ftb
            }
            s0PC = nextPC;
        } else {
            DPRINTF(DecoupleBP || debugFlagOn, "Prediction is not reasonable, printing ftb entry\n");
            ftb->printFTBEntry(finalPred.ftbEntry);
            dbpFtbStats.predFalseHit++;
            // prediction is not reasonable, use fall through
            entry.isHit = false;
            entry.falseHit = true;
            entry.predTaken = false;
            entry.predEndPC = entry.startPC + 32;
            entry.predFTBEntry = FTBEntry();
            s0PC = entry.startPC + 32; // TODO: parameterize
            // TODO: when false hit, act like a miss, do not update history
        }

        // jump ahead lookups
        if (enableJumpAheadPredictor) {
            bool jaHit = false;
            bool jaConf = false;
            JAEntry jaEntry;
            Addr jaTarget;
            std::tie(jaHit, jaConf, jaEntry, jaTarget) = jap.lookup(entry.startPC);
            // ensure this block does not hit ftb
            if (!finalPred.valid) {
                if (jaHit && jaConf) {
                    entry.jaHit = true;
                    entry.predEndPC = jaTarget;
                    entry.jaEntry = jaEntry;
                    entry.currentSentBlock = 0;
                    s0PC = jaTarget;
                    dbpFtbStats.predJATotalSkippedBlocks += jaEntry.jumpAheadBlockNum - 1;
                    dbpFtbStats.predJASkippedBlockNum.sample(jaEntry.jumpAheadBlockNum - 1, 1);
                }
            }
        }


        entry.history = s0History;
        entry.predTick = finalPred.predTick;
        entry.predCycle = finalPred.predCycle;
        entry.predSource = finalPred.predSource;

        // update (folded) histories for components
        for (int i = 0; i < numComponents; i++) {
            components[i]->specUpdateHist(s0History, finalPred);
            entry.predMetas[i] = components[i]->getPredictionMeta();
        }
        // update ghr
        int shamt;
        std::tie(shamt, taken) = finalPred.getHistInfo();
        histShiftIn(shamt, taken, s0History);

        historyManager.addSpeculativeHist(entry.startPC, shamt, taken, entry.predBranchInfo, fsqId);
        tage->checkFoldedHist(s0History, "speculative update");

        
        entry.setDefaultResolve();


    } else {
        assert(enableLoopPredictor);
        // loop buffer is activated, use loop buffer to make prediction
        // determine whether this stream entry has double iterations
        entry = lb.streamBeforeLoop;
        bool mayBeDouble = lb.activeLoopMayBeDouble();
        // this is a condition read from lp while activating lb
        bool loopBranchConf = lb.currentLoopBranchConf();
        bool twoOrMoreItersRemaining = false;
        std::tie(endLoop, lpRedirectInfos[0], twoOrMoreItersRemaining, loopConf) = lp.shouldEndLoop(
            true, lb.getActiveLoopBranch(), mayBeDouble
        );
        bool confExit = false;
        bool provideDoubleBlock = false;
        // loop branch confident in lp when activating lb
        // let lp decide whether to end loop
        if (loopBranchConf) {
            bool lpConfNow = loopConf;
            confExit = lpConfNow && endLoop;
            provideDoubleBlock = twoOrMoreItersRemaining || !lpConfNow;
            entry.isExit = confExit;
            entry.predTaken = twoOrMoreItersRemaining || !confExit;
        } else {
            // loop branch not confident in lp when activating lb
            // do not proactively deactivate loop buffer

            // provide double block if possible
            provideDoubleBlock = mayBeDouble;
            // do not exit until squash
            entry.isExit = false;
            entry.predTaken = true;
        }
        entry.isDouble = provideDoubleBlock;
        entry.startPC = s0PC;
        entry.fromLoopBuffer = true;
        entry.isHit = true;
        entry.falseHit = false;
        entry.predEndPC = lb.streamBeforeLoop.predBranchInfo.getEnd();
        // use s0History from streamBeforeLoop
        // entry.history = s0History;
        entry.predTick = curTick();
        entry.predCycle = curCycle();
        entry.predSource = numStages;

        // TODO: use what kind of mechanism to handle ghr?
        // use default meta from streamBeforeLoop here
        // for (int i = 0; i < numComponents; i++) {
        //     entry.predMetas[i] = components[i]->getPredictionMeta();
        // }
        int shamt = 0;
        bool taken = false;
        histShiftIn(shamt, taken, s0History);
        historyManager.addSpeculativeHist(entry.startPC, shamt, taken, entry.predBranchInfo, fsqId);
        tage->checkFoldedHist(s0History, "speculative update");
        entry.setDefaultResolve();
        


        // redirect to fall through of loop branch if loop is ended
        if (confExit) {
            s0PC = lb.streamBeforeLoop.predBranchInfo.getEnd();
            lb.deactivate(false);
        }


        if (endLoop && !loopConf) {
            dbpFtbStats.predLoopPredictorUnconfNotExit++;
        }
        if (confExit) {
            dbpFtbStats.predLoopPredictorExit++;
        }
        dbpFtbStats.predBlockInLoopBuffer++;
        if (provideDoubleBlock) {
            dbpFtbStats.predDoubleBlockInLoopBuffer++;
        }
    }
    entry.loopRedirectInfos = lpRedirectInfos;
    entry.fixNotExits = fixNotExits;
    entry.unseenLoopRedirectInfos = unseenLpRedirectInfos;

    DPRINTF(LoopBuffer, "previous stream before loop:\n");
    printStream(lb.streamBeforeLoop);
    if (enableLoopBuffer && !lb.isActive()) {
        lb.recordNewestStreamOutsideLoop(entry);
    }
    DPRINTF(LoopBuffer, "now stream before loop:\n");
    printStream(lb.streamBeforeLoop);


    streamToEnqueue = entry;
}


void
DecoupledBPUWithFTB::enqueueFetchStream()
{

    auto [insert_it, inserted] = fetchStreamQueue.emplace(fsqId, streamToEnqueue);
    assert(inserted);

    dumpFsq("after insert new stream");
    DPRINTF(DecoupleBP || debugFlagOn, "Insert fetch stream %lu\n", fsqId);

    fsqId++;
    printStream(streamToEnqueue);

    dbpFtbStats.fsqEntryEnqueued++;
}

void
DecoupledBPUWithFTB::checkHistory(const boost::dynamic_bitset<> &history)
{/*
    unsigned ideal_size = 0;
    boost::dynamic_bitset<> ideal_hash_hist(historyBits, 0);
    int ideal_sp = ras->getNonSpecSp();
    std::vector<RAS::RASEntry> ideal_stack(ras->getNonSpecStack());
    for (const auto entry: historyManager.getSpeculativeHist()) {
        if (entry.shamt == 0 && !entry.is_call && !entry.is_return) {
            continue;
        }
        if (entry.shamt != 0) {
            ideal_size += entry.shamt;
            DPRINTF(DecoupleBPVerbose, "pc: %#lx, shamt: %d, cond_taken: %d\n", entry.pc,
                    entry.shamt, entry.cond_taken);
            ideal_hash_hist <<= entry.shamt;
            ideal_hash_hist[0] = entry.cond_taken;
        }
        if (entry.is_call || entry.is_return) {
            DPRINTF(DecoupleBPVerbose, "pc: %#lx, is_call: %d, is_return: %d, retAddr: %#lx\n",
                entry.pc, entry.is_call, entry.is_return, entry.retAddr);
            if (entry.is_call) {
                ras->push(entry.retAddr, ideal_stack, ideal_sp);
            }
            if (entry.is_return) {
                ras->pop(ideal_stack, ideal_sp);
            }
        }
    }
    unsigned comparable_size = std::min(ideal_size, historyBits);
    boost::dynamic_bitset<> sized_real_hist(history);
    ideal_hash_hist.resize(comparable_size);
    sized_real_hist.resize(comparable_size);

    DPRINTF(DecoupleBP,
            "Ideal size:\t%u, real history size:\t%u, comparable size:\t%u\n",
            ideal_size, historyBits, comparable_size);
    DPRINTF(DecoupleBP, "Ideal history:\t%s\nreal history:\t%s\n",
            buf1.c_str(), buf2.c_str());
    int sp = ras->getSp();
    DPRINTF(DecoupleBP, "ideal sp:\t%d, real sp:\t%d\n", ideal_sp, sp);
    assert(ideal_hash_hist == sized_real_hist);
    // assert(ideal_sp == sp);
    */
}

void
DecoupledBPUWithFTB::resetPC(Addr new_pc)
{
    s0PC = new_pc;
    fetchTargetQueue.resetPC(new_pc);
}

Cycles
DecoupledBPUWithFTB::curCycle()
{
    return cpu->curCycle();
}

Addr
DecoupledBPUWithFTB::getPreservedReturnAddr(const DynInstPtr &dynInst)
{
    DPRINTF(DecoupleBP, "acquiring reutrn address for inst pc %#lx from decode\n", dynInst->pcState().instAddr());
    auto fsqid = dynInst->getFsqId();
    auto it = fetchStreamQueue.find(fsqid);
    auto retAddr = ras->getTopAddrFromMetas(it->second);
    DPRINTF(DecoupleBP, "get ret addr %#lx\n", retAddr);
    return retAddr;
}

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
