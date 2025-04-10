#include "cpu/pred/btb/decoupled_bpred.hh"

#include "base/output.hh"
#include "base/debug_helper.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/btb/stream_common.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/DecoupleBPHist.hh"
#include "debug/Override.hh"
#include "debug/BTB.hh"
#include "debug/ITTAGE.hh"
#include "debug/JumpAheadPredictor.hh"
#include "debug/Profiling.hh"
#include "sim/core.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

DecoupledBPUWithBTB::DecoupledBPUWithBTB(const DecoupledBPUWithBTBParams &p)
    : BPredUnit(p),
      enableLoopBuffer(p.enableLoopBuffer),
      enableLoopPredictor(p.enableLoopPredictor),
      enableJumpAheadPredictor(p.enableJumpAheadPredictor),
      fetchTargetQueue(p.ftq_size),
      fetchStreamQueueSize(p.fsq_size),
      alignToBlockSize(p.alignToBlockSize),
      historyBits(p.maxHistLen),
      phrbMaxLen(p.phrbMaxLen),
      phrbXorLen(p.phrbXorLen),
      phrtMaxLen(p.phrtMaxLen),
      phrtXorLen(p.phrtXorLen),
      ubtb(p.ubtb),
      abtb(p.abtb),
      btb(p.btb),
      tage(p.tage),
      ittage(p.ittage),
      ras(p.ras),
    //   uras(p.uras),
      bpDBSwitches(p.bpDBSwitches),
      numStages(p.numStages),
      historyManager(8), // TODO: fix this
      dbpBtbStats(this, p.numStages, p.fsq_size)
{
    btb_pred::predictWidth = p.predictWidth;  // set global variable, used in stream_struct.hh
    btb_pred::alignToBlockSize = p.alignToBlockSize;
    numBr = 8; //TODO: remove numBr
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
    bpType = DecoupledBTBType;
    numStages = 3;
    // TODO: better impl (use vector to assign in python)
    // problem: btb->getAndSetNewBTBEntry
    components.push_back(ubtb);
    components.push_back(abtb);
    // components.push_back(uras);
    components.push_back(btb);
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
        clearPreds();
    }

    s0PC = 0x80000000;

    s0History.resize(historyBits, 0);
    s0phrb.resize(phrbMaxLen, 0);
    s0phrt.resize(phrtMaxLen, 0);
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
                return gem5::branch_prediction::btb_pred::DecoupledBPUWithBTB::getMispredCount(a) >
                gem5::branch_prediction::btb_pred::DecoupledBPUWithBTB::getMispredCount(b);
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
                return gem5::branch_prediction::btb_pred::DecoupledBPUWithBTB::getMispredCount(a) >
                gem5::branch_prediction::btb_pred::DecoupledBPUWithBTB::getMispredCount(b);
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

        // dump btb entries
        int outputTopNEntries = 1;
        out_handle = simout.create("btbEntriesByPhase.txt", false, true);
        *out_handle->stream() << "phaseID"<< " " << "numBTBEntries";
        for (int i = 0; i <= outputTopNEntries; i++) {
            *out_handle->stream() << " " << "entry_" << i << "_pc" << " " << "entry_" << i << "_type";
        }
        *out_handle->stream() << std::endl;
        phaseID = 0;

        for (auto& it : BTBEntriesByPhase) {
            *out_handle->stream() << std::dec << phaseID;
            *out_handle->stream() << " " << it.size();
            std::vector<std::tuple<Addr, BTBEntry, int>> btbEntryTempVec;
            for (auto& rec : it) {
                btbEntryTempVec.push_back(std::make_tuple(rec.first, rec.second.first, rec.second.second));
            }
            std::sort(btbEntryTempVec.begin(), btbEntryTempVec.end(),
                [](const std::tuple<Addr, BTBEntry, int> &a,
                const std::tuple<Addr, BTBEntry, int> &b) {
                    return std::get<2>(a) > std::get<2>(b);
                });
            for (int i = 0; i <= outputTopNEntries && i < btbEntryTempVec.size(); i++) {
                auto &rec = btbEntryTempVec[i];
                *out_handle->stream() << " " << std::hex << std::get<0>(rec) << " " << std::get<1>(rec).getType();
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

DecoupledBPUWithBTB::DBPBTBStats::DBPBTBStats(statistics::Group* parent, unsigned numStages, unsigned fsqSize):
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
    ADD_STAT(overrideBubbleNum,  statistics::units::Count::get(), "the number of override bubbles"),
    ADD_STAT(overrideCount, statistics::units::Count::get(), "the number of overrides"),
    ADD_STAT(overrideValidityMismatch, statistics::units::Count::get(),
    "Number of overrides due to validity mismatches"),
    ADD_STAT(overrideControlAddrMismatch, statistics::units::Count::get(),
    "Number of overrides due to control address mismatches"),
    ADD_STAT(overrideTargetMismatch, statistics::units::Count::get(),"Number of overrides due to target mismatches"),
    ADD_STAT(overrideEndMismatch, statistics::units::Count::get(),"Number of overrides due to end address mismatches"),
    ADD_STAT(overrideHistInfoMismatch, statistics::units::Count::get(),
    "Number of overrides due to history info mismatches"),
    ADD_STAT(commitPredsFromEachStage, statistics::units::Count::get(),
    "the number of preds of each stage that account for a committed stream"),
    ADD_STAT(commitOverrideBubbleNum, statistics::units::Count::get(),
    "the number of override bubbles, on the commit path"),
    ADD_STAT(commitOverrideCount, statistics::units::Count::get(), "the number of overrides, on the commit path"),
    ADD_STAT(fsqEntryDist, statistics::units::Count::get(), "the distribution of number of entries in fsq"),
    ADD_STAT(fsqEntryEnqueued, statistics::units::Count::get(), "the number of fsq entries enqueued"),
    ADD_STAT(fsqEntryCommitted, statistics::units::Count::get(), "the number of fsq entries committed at last"),
    ADD_STAT(controlSquash, statistics::units::Count::get(), "the number of control squashes in bpu"),
    ADD_STAT(nonControlSquash, statistics::units::Count::get(), "the number of non-control squashes in bpu"),
    ADD_STAT(trapSquash, statistics::units::Count::get(), "the number of trap squashes in bpu"),
    ADD_STAT(ftqNotValid, statistics::units::Count::get(), "fetch needs ftq req but ftq not valid"),
    ADD_STAT(fsqNotValid, statistics::units::Count::get(), "ftq needs fsq req but fsq not valid"),
    ADD_STAT(fsqFullCannotEnq, statistics::units::Count::get(), "bpu has req but fsq full cannot enqueue"),
    ADD_STAT(commitFsqEntryHasInsts, statistics::units::Count::get(), "number of insts that commit fsq entries have"),
    ADD_STAT(commitFsqEntryFetchedInsts, statistics::units::Count::get(), "number of insts that commit fsq entries fetched"),
    ADD_STAT(commitFsqEntryOnlyHasOneJump, statistics::units::Count::get(), "number of fsq entries with only one instruction (jump)"),
    ADD_STAT(btbHit, statistics::units::Count::get(), "btb hits (in predict block)"),
    ADD_STAT(btbMiss, statistics::units::Count::get(), "btb misses (in predict block)"),
    ADD_STAT(btbEntriesWithDifferentStart, statistics::units::Count::get(), "number of btb entries with different start PC"),
    ADD_STAT(btbEntriesWithOnlyOneJump, statistics::units::Count::get(), "number of btb entries with different start PC starting with a jump"),
    ADD_STAT(predFalseHit, statistics::units::Count::get(), "false hit detected at pred"),
    ADD_STAT(commitFalseHit, statistics::units::Count::get(), "false hit detected at commit"),
    ADD_STAT(predLoopPredictorExit, statistics::units::Count::get(), "loop predictor exits at pred"),
    ADD_STAT(predLoopPredictorUnconfNotExit, statistics::units::Count::get(), "loop predictor does not exit at pred because of unconf"),
    ADD_STAT(predLoopPredictorConfFixNotExit, statistics::units::Count::get(), "loop predictor confident and fix other predictor not taken in non-exit loop branch at pred"),
    ADD_STAT(predBTBUnseenLoopBranchInLp, statistics::units::Count::get(), "loop predictor recorded loop branch that is not in btb encountered"),
    ADD_STAT(predBTBUnseenLoopBranchExitInLp, statistics::units::Count::get(), "loop predictor recorded loop branch that is not in btb encountered and exited"),
    ADD_STAT(commitLoopPredictorExit, statistics::units::Count::get(), "loop predictor pred loop exits detected at commit"),
    ADD_STAT(commitLoopPredictorExitCorrect, statistics::units::Count::get(), "loop predictor correctly pred loop exits detected at commit"),
    ADD_STAT(commitLoopPredictorExitWrong, statistics::units::Count::get(), "loop predictor wrongly pred loop exits detected at commit"),
    ADD_STAT(commitBTBUnseenLoopBranchInLp, statistics::units::Count::get(), "loop predictor recorded loop branch that is not in btb encountered at commit"),
    ADD_STAT(commitBTBUnseenLoopBranchExitInLp, statistics::units::Count::get(), "loop predictor recorded loop branch that is not in btb encountered and exited at commit"),
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
    ADD_STAT(commitJASkippedBlockNum, statistics::units::Count::get(), "distribution of ja skipped block numbers at commit")
{
    predsOfEachStage.init(numStages);
    commitPredsFromEachStage.init(numStages+1);
    commitOverrideBubbleNum = commitPredsFromEachStage[1] + 2 * commitPredsFromEachStage[2] ;
    commitOverrideCount = commitPredsFromEachStage[1] + commitPredsFromEachStage[2];
    fsqEntryDist.init(0, fsqSize, 1);
    commitLoopBufferEntryInstNum.init(0, 16, 1);
    commitLoopBufferDoubleEntryInstNum.init(0, 16, 1);
    commitFsqEntryHasInsts.init(0, 16, 1);
    commitFsqEntryFetchedInsts.init(0, 16, 1);
    predJASkippedBlockNum.init(0, 16, 1);
    commitJASkippedBlockNum.init(0, 16, 1);
}

DecoupledBPUWithBTB::BpTrace::BpTrace(FetchStream &stream, const DynInstPtr &inst, bool mispred)
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
DecoupledBPUWithBTB::tick()
{
    // Monitor FSQ size for statistics
    dbpBtbStats.fsqEntryDist.sample(fetchStreamQueue.size(), 1);
    if (streamQueueFull()) {
        dbpBtbStats.fsqFullCannotEnq++;
        DPRINTF(Override, "FSQ is full (%lu entries)\n", fetchStreamQueue.size());
    }

    // Generate final prediction if we have received PC and history but no prediction yet
    if (!receivedPred && numOverrideBubbles == 0 && sentPCHist) {
        DPRINTF(Override, "Generating final prediction for PC %#lx\n", s0PC);
        generateFinalPredAndCreateBubbles();
    }

    // Try to enqueue new predictions if not squashing
    if (!squashing) {
        DPRINTF(Override, "DecoupledBPUWithBTB::tick()\n");
        tryEnqFetchTarget();
        tryEnqFetchStream();
    } else {
        receivedPred = false;
        DPRINTF(Override, "Squashing, skip this cycle, receivedPred is %d.\n", receivedPred);
    }

    // Decrement override bubbles counter
    if (numOverrideBubbles > 0) {
        numOverrideBubbles--;
        dbpBtbStats.overrideBubbleNum++;
        DPRINTF(Override, "Consuming override bubble, %d remaining\n", numOverrideBubbles);
    }

    sentPCHist = false;

    // Request new prediction if FSQ not full and not using loop buffer
    if (!receivedPred && !streamQueueFull()) {
        if (!enableLoopBuffer || (enableLoopBuffer && !lb.isActive())) {
            DPRINTF(Override, "Requesting new prediction for PC %#lx\n", s0PC);
            
            // Initialize prediction state for each stage
            for (int i = 0; i < numStages; i++) {
                predsOfEachStage[i].bbStart = s0PC;
            }
            tage->putPhr(s0phrb, s0phrt);
            // Query each predictor component with current PC and history
            for (int i = 0; i < numComponents; i++) {
                components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);
            }
        } else {
            DPRINTF(DecoupleBP, "Loop buffer active - skipping predictor query\n");
        }

        // Mark that we've sent PC and history to predictors
        sentPCHist = true;
    }
    

    // query loop buffer with start pc
    if (enableLoopBuffer && !lb.isActive() &&
            lb.streamBeforeLoop.getTakenTarget() == lb.streamBeforeLoop.startPC &&
            !lb.streamBeforeLoop.resolved) { // Don't activate after squash
        DPRINTF(LoopBuffer, "Attempting to activate loop buffer for PC %#lx\n", s0PC);
        lb.tryActivateLoop(s0PC);
    }

    DPRINTF(Override, "Prediction cycle complete\n");
    
    // Clear squashing state for next cycle
    squashing = false;
}

void DecoupledBPUWithBTB::OverrideStats(OverrideReason overrideReason)
{
    if (numOverrideBubbles > 0) {
        dbpBtbStats.overrideCount++;
        
        // Track specific override reasons for statistics
        switch (overrideReason) {
            case OverrideReason::validity:
                dbpBtbStats.overrideValidityMismatch++;
                break;
            case OverrideReason::controlAddr:
                dbpBtbStats.overrideControlAddrMismatch++;
                break;
            case OverrideReason::target:
                dbpBtbStats.overrideTargetMismatch++;
                break;
            case OverrideReason::end:
                dbpBtbStats.overrideEndMismatch++;
                break;
            case OverrideReason::histInfo:
                dbpBtbStats.overrideHistInfoMismatch++;
                break;
            default:
                break;
        }
    }
}

// this function collects predictions from all stages and generate bubbles
// when loop buffer is active, predictions are from saved stream
void
DecoupledBPUWithBTB::generateFinalPredAndCreateBubbles()
{
    DPRINTF(Override, "In generateFinalPredAndCreateBubbles().\n");

    // If loop buffer is active, skip normal prediction process
    if (enableLoopBuffer && lb.isActive()) {
        numOverrideBubbles = 0;
        receivedPred = true;
        DPRINTF(DecoupleBP, "Loop buffer active - skipping normal prediction process\n");
        return;
    }

    // 1. Debug output: dump predictions from all stages
    for (int i = 0; i < numStages; i++) {
        printFullBTBPrediction(predsOfEachStage[i]);
    }

    // 2. Select the most accurate prediction (prioritize later stages)
    // Initially assume stage 0 (UBTB) prediction
    FullBTBPrediction *chosenPrediction = &predsOfEachStage[0];

    // Search from last stage to first for valid predictions
    for (int i = (int)numStages - 1; i >= 0; i--) {
        if (predsOfEachStage[i].btbEntries.size() > 0) {
            chosenPrediction = &predsOfEachStage[i];
            DPRINTF(Override, "Selected prediction from stage %d\n", i);
            break;
        }
    }

    // Store the chosen prediction as our final prediction
    finalPred = *chosenPrediction;

    // 3. Calculate override bubbles needed for pipeline consistency
    // Override bubbles are needed when earlier stages predict differently from later stages
    unsigned first_hit_stage = 0;
    OverrideReason overrideReason = OverrideReason::no_override;

    // Find first stage that matches the chosen prediction
    while (first_hit_stage < numStages - 1) {
        auto [matches, reason] = predsOfEachStage[first_hit_stage].match(*chosenPrediction);
        if (matches) {
            break;
        }
        first_hit_stage++;
        overrideReason = reason;
    }

    // 4. Record override bubbles and update statistics
    numOverrideBubbles = first_hit_stage;
    OverrideStats(overrideReason);

    // 5. Finalize prediction process
    finalPred.predSource = first_hit_stage;
    receivedPred = true;

    // Debug output for final prediction
    printFullBTBPrediction(finalPred);
    // dbpBtbStats.predsOfEachStage[first_hit_stage]++;

    // Clear stage predictions for next cycle
    clearPreds();

    DPRINTF(Override, "Prediction complete: override bubbles=%d, receivedPred=true\n", 
            numOverrideBubbles);
}

bool
DecoupledBPUWithBTB::trySupplyFetchWithTarget(Addr fetch_demand_pc, bool &fetch_target_in_loop)
{
    return fetchTargetQueue.trySupplyFetchWithTarget(fetch_demand_pc, fetch_target_in_loop);
}

std::pair<bool, bool>
DecoupledBPUWithBTB::decoupledPredict(const StaticInstPtr &inst,
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
                "Predicted pc: %#lx, upc: %u, npc(meaningless): %#lx, instSeqNum: %lu\n",
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
DecoupledBPUWithBTB::controlSquash(unsigned target_id, unsigned stream_id,
                            const PCStateBase &control_pc,
                            const PCStateBase &corr_target,
                            const StaticInstPtr &static_inst,
                            unsigned control_inst_size, bool actually_taken,
                            const InstSeqNum &seq, ThreadID tid,
                            const unsigned &currentLoopIter, const bool fromCommit)
{
    dbpBtbStats.controlSquash++;

    // Get branch type information
    bool is_conditional = static_inst->isCondCtrl();
    bool is_indirect = static_inst->isIndirectCtrl();
    // bool is_call = static_inst->isCall() && !static_inst->isNonSpeculative();
    // bool is_return = static_inst->isReturn() && !static_inst->isNonSpeculative();




    squashing = true;

    // Find the stream being squashed
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

    // Create branch info for squash
    auto squashBranchInfo = BranchInfo(control_pc.instAddr(), real_target, static_inst, control_inst_size);
    if (stream.isExit) {
        dbpBtbStats.controlSquashOnLoopPredictorPredExit++;
    }
    if (stream.fromLoopBuffer) {
        dbpBtbStats.squashOnLoopBufferPredBlock++;
        if (stream.isDouble) {
            dbpBtbStats.squashOnLoopBufferDoublePredBlock++;
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
            "stream start=%#lx, predict on hist\n", stream.startPC);

    DPRINTF(DecoupleBP || debugFlagOn,
            "Control squash: ftq_id=%d, fsq_id=%d,"
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
        dbpBtbStats.controlSquashOnJaHitBlocks++;
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
    // DPRINTF(DecoupleBPHist,
    //          "Recover history %s\nto %s\n", s0History, stream.history);
    s0History = stream.history;
    s0phrb = stream.phrb;
    s0phrt = stream.phrt;

    // update phr
    if (actually_taken) {
        updatePhr(stream.startPC, real_target);
    }

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

    // DPRINTF(DecoupleBPHist,
    //             "Shift in history %s\n", s0History);

    printStream(stream);

    if (enableLoopBuffer) {
        lb.recordNewestStreamOutsideLoop(stream);
    }

    clearPreds();

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
DecoupledBPUWithBTB::nonControlSquash(unsigned target_id, unsigned stream_id,
                               const PCStateBase &inst_pc,
                               const InstSeqNum seq, ThreadID tid, const unsigned &currentLoopIter)
{
    dbpBtbStats.nonControlSquash++;
    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
            "non control squash: target id: %d, stream id: %d, inst_pc: %#lx, "
            "seq: %lu\n",
            target_id, stream_id, inst_pc.instAddr(), seq);
    squashing = true;

    dumpFsq("before non-control squash");

    // make sure the stream is in FSQ
    auto it = fetchStreamQueue.find(stream_id);
    assert(it != fetchStreamQueue.end());

    auto ftq_demand_stream_id = stream_id;
    auto &stream = it->second;

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
        dbpBtbStats.nonControlSquashOnLoopPredictorPredExit++;
    }
    if (stream.fromLoopBuffer) {
        dbpBtbStats.squashOnLoopBufferPredBlock++;
        if (stream.isDouble) {
            dbpBtbStats.squashOnLoopBufferDoublePredBlock++;
        }
    }

    stream.exeTaken = false;
    stream.resolved = true;
    stream.squashPC = inst_pc.instAddr();
    stream.squashType = SQUASH_OTHER;

    if (enableJumpAheadPredictor && stream.jaHit) {
        dbpBtbStats.nonControlSquashOnJaHitBlocks++;
    }

    // recover history info
    s0History = it->second.history;
    s0phrb = stream.phrb;
    s0phrt = stream.phrt;

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
    clearPreds();

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
DecoupledBPUWithBTB::trapSquash(unsigned target_id, unsigned stream_id,
                         Addr last_committed_pc, const PCStateBase &inst_pc,
                         ThreadID tid, const unsigned &currentLoopIter)
{
    dbpBtbStats.trapSquash++;
    DPRINTF(DecoupleBP || debugFlagOn,
            "Trap squash: target id: %d, stream id: %d, inst_pc: %#lx\n",
            target_id, stream_id, inst_pc.instAddr());
    squashing = true;

    auto pc = inst_pc.instAddr();

    if (pc == ObservingPC) dumpFsq("before trap squash");

    auto it = fetchStreamQueue.find(stream_id);
    assert(it != fetchStreamQueue.end());
    auto &stream = it->second;

    if (stream.isExit) {
        dbpBtbStats.trapSquashOnLoopPredictorPredExit++;
    }
    if (stream.fromLoopBuffer) {
        dbpBtbStats.squashOnLoopBufferPredBlock++;
        if (stream.isDouble) {
            dbpBtbStats.squashOnLoopBufferDoublePredBlock++;
        }
    }

    stream.resolved = true;
    stream.exeTaken = false;
    stream.squashPC = inst_pc.instAddr();
    stream.squashType = SQUASH_TRAP;

    if (enableJumpAheadPredictor && stream.jaHit) {
        dbpBtbStats.trapSquashOnJaHitBlocks++;
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
    s0phrb = stream.phrb;
    s0phrt = stream.phrt;

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
    clearPreds();

    s0PC = inst_pc.instAddr();

    DPRINTF(DecoupleBP,
            "After trap squash, FSQ head Id=%lu, s0pc=%#lx, demand stream "
            "Id=%lu, Fetch demanded target Id=%lu\n",
            fsqId, s0PC, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());
}

void DecoupledBPUWithBTB::update(unsigned stream_id, ThreadID tid)
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
            DPRINTF(ITTAGE || (stream.squashPC == 0x1e0eb6), "miss predicted stream.startAddr=%#lx\n", stream.startPC);
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
        
        if (stream.isHit) {
            // FIXME: should count in terms of instruction instead of block
            dbpBtbStats.btbHit++;
        } else {
            if (stream.exeTaken) {
                dbpBtbStats.btbMiss++;
                DPRINTF(BTB, "BTB miss detected when update, stream start %#lx, predTick %lu, printing branch info:\n", stream.startPC, stream.predTick);
                auto &slot = stream.exeBranchInfo;
                DPRINTF(BTB, "    pc:%#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d\n",
                slot.pc, slot.size, slot.target, slot.isCond, slot.isIndirect, slot.isCall, slot.isReturn);
            }
            if (stream.falseHit) {
                dbpBtbStats.commitFalseHit++;
            }
        }
        dbpBtbStats.commitPredsFromEachStage[stream.predSource]++;


        if (stream.isHit || stream.exeTaken) {
            stream.setUpdateInstEndPC(predictWidth);
            stream.setUpdateBTBEntries();
            //abtb->getAndSetNewBTBEntry(stream);
            btb->getAndSetNewBTBEntry(stream);
            for (int i = 0; i < numComponents; ++i) {
                components[i]->update(stream);
            }
            // btb entry stats
            auto it = totalBTBEntries.find(stream.startPC);
            if (it == totalBTBEntries.end()) {
                auto &btb_entry = stream.updateNewBTBEntry;
                totalBTBEntries[stream.startPC] = std::make_pair(btb_entry, 1);
                dbpBtbStats.btbEntriesWithDifferentStart++;
            } else {
                it->second.second++;
                it->second.first = stream.updateNewBTBEntry;
            }
        }

        if (enableJumpAheadPredictor) {
            if (stream.isHit || stream.exeTaken || stream.squashType != SQUASH_NONE) {
                // this block predicted, if we already recorded enough non-pred blocks,
                // then we should write into storage
                // update predicted blocks of ja info
                jap.tryUpdate(jaInfo, stream.startPC);
                jaInfo.setPredictedBlock(stream.startPC, stream.updateNewBTBEntry);
            } else {
                // this block has no pred, increment non-pred count
                jaInfo.incrementNoPredBlockCount(stream.startPC);
            }

            // do some statistics
            if (stream.jaHit) {
                int skippedBlocks = stream.jaEntry.jumpAheadBlockNum - 1;
                dbpBtbStats.commitJATotalSkippedBlocks += skippedBlocks;
                dbpBtbStats.commitJASkippedBlockNum.sample(skippedBlocks, 1);
                switch (stream.squashType) {
                    case SQUASH_CTRL:
                        dbpBtbStats.commitControlSquashedOnJaHitBlocks++;
                        break;
                    case SQUASH_OTHER:
                        dbpBtbStats.commitNonControlSquashedOnJaHitBlocks++;
                        break;
                    case SQUASH_TRAP:
                        dbpBtbStats.commitTrapSquashedOnJaHitBlocks++;
                        break;
                    default:
                        break;
                }
            }
        }


        // check loop predictor prediction
        if (enableLoopPredictor) {

            auto lp_infos = stream.loopRedirectInfos;
            DPRINTF(LoopPredictor, "at commit fsqid %d, real_branch_pc %#lx, squash type %d, loop predcition infos:\n", it->first, stream.exeBranchInfo.pc, stream.squashType);
            DPRINTF(LoopBuffer, "from loop buffer %d, doubling %d, exit %d\n", stream.fromLoopBuffer, stream.isDouble, stream.isExit);
            for (int i = 0; i < numBr; ++i) {
                auto &lp_info = lp_infos[i];
                DPRINTF(LoopPredictor, "    branch_pc %#lx, end_loop %d, specCnt %d, tripCnt %d, conf %d\n",
                    lp_info.branch_pc, lp_info.end_loop, lp_info.e.specCnt, lp_info.e.tripCnt, lp_info.e.conf);
                if (stream.fixNotExits[i]) {
                    dbpBtbStats.commitLoopPredictorConfFixNotExit++;
                    if (stream.squashType == SQUASH_CTRL && stream.squashPC == lp_info.branch_pc) {
                        dbpBtbStats.commitLoopPredictorConfFixNotExitWrong++;
                    }
                    if (stream.squashType != SQUASH_CTRL ||
                        (stream.squashType == SQUASH_CTRL && stream.squashPC != lp_info.branch_pc))
                    {
                        dbpBtbStats.commitLoopPredictorConfFixNotExitCorrect++;
                    }
                }
            }
            if (stream.isExit) {
                dbpBtbStats.commitLoopPredictorExit++;
                if (stream.squashType == SQUASH_NONE) {
                    dbpBtbStats.commitLoopPredictorExitCorrect++;
                } else if (stream.squashType == SQUASH_CTRL) {
                    // FIXME: distinguish between squash of other branches
                    dbpBtbStats.commitLoopPredictorExitWrong++;
                }
            }
        }
        
        if (enableLoopBuffer) {
            if (stream.fromLoopBuffer) {
                dbpBtbStats.commitBlockInLoopBuffer++;
                if (stream.isDouble) {
                    dbpBtbStats.commitDoubleBlockInLoopBuffer++;
                }
                if (stream.squashType != SQUASH_NONE) {
                    dbpBtbStats.commitBlockInLoopBufferSquashed++;
                    if (stream.isDouble) {
                        dbpBtbStats.commitDoubleBlockInLoopBufferSquashed++;
                    }
                }
                auto instNum = lb.getLoopInstNum(stream.startPC);
                if (instNum > 0) {
                    dbpBtbStats.commitLoopBufferEntryInstNum.sample(instNum, 1);
                    if (stream.isDouble) {
                        dbpBtbStats.commitLoopBufferDoubleEntryInstNum.sample(instNum, 1);
                    }
                }
            }
        }

        dbpBtbStats.commitFsqEntryHasInsts.sample(stream.commitInstNum, 1);
        if (stream.commitInstNum >= 0 && stream.commitInstNum <= 16) {
            commitFsqEntryHasInstsVector[stream.commitInstNum]++;
            if (stream.commitInstNum == 1 && stream.exeBranchInfo.isUncond()) {
                dbpBtbStats.commitFsqEntryOnlyHasOneJump++;
            }
        }
        dbpBtbStats.commitFsqEntryFetchedInsts.sample(stream.fetchInstNum, 1);
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

        dbpBtbStats.fsqEntryCommitted++;
    }
    DPRINTF(DecoupleBP, "after commit stream, fetchStreamQueue size: %lu\n",
            fetchStreamQueue.size());
    printStream(it->second);

    historyManager.commit(stream_id);
}

void
DecoupledBPUWithBTB::commitBranch(const DynInstPtr &inst, bool miss)
{
    // do overall statistics
    if (inst->isUncondCtrl()) {
        addCfi(branch_prediction::btb_pred::DecoupledBPUWithBTB::CfiType::UNCOND, miss);
    }
    if (inst->isCondCtrl()) {
        addCfi(branch_prediction::btb_pred::DecoupledBPUWithBTB::CfiType::COND, miss);
    }
    if (inst->isReturn()) {
        addCfi(branch_prediction::btb_pred::DecoupledBPUWithBTB::CfiType::RETURN, miss);
    } else if (inst->isIndirectCtrl()) {
        addCfi(branch_prediction::btb_pred::DecoupledBPUWithBTB::CfiType::OTHER, miss);
    }
    DPRINTF(DBPBTBStats, "inst=%s\n", inst->staticInst->disassemble(inst->pcState().instAddr()));
    DPRINTF(DBPBTBStats, "isUncondCtrl=%d, isCondCtrl=%d, isReturn=%d, isIndirectCtrl=%d\n",
            inst->isUncondCtrl(), inst->isCondCtrl(), inst->isReturn(), inst->isIndirectCtrl());

    // break down into each predictor and each stage
    // find corresponding fsq entry first
    auto it = fetchStreamQueue.find(inst->fsqId);
    assert(it != fetchStreamQueue.end());
    auto entry = it->second;
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
            bool predBranchInBTB = false;
            for (auto &e: entry.predBTBEntries) {
                if (e.pc == branchAddr) {
                    predBranchInBTB = true;
                    break;
                }
            }
            if (!predBranchInBTB) {
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
        dbpBtbStats.staticBranchNum++;
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
            dbpBtbStats.staticBranchNumEverTaken++;
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


    if (enableLoopPredictor) {
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
                        dbpBtbStats.commitLoopExitLoopPredictorNotConf++;
                    }
                } else {
                    dbpBtbStats.commitLoopExitLoopPredictorNotPredicted++;
                }
            }
        }
        for (auto &info : entry.unseenLoopRedirectInfos) {
            if (info.branch_pc == inst->pcState().instAddr()) {
                auto &loopEntry = info.e;
                dbpBtbStats.commitBTBUnseenLoopBranchInLp++;
                if (loopEntry.specCnt == loopEntry.tripCnt) {
                    dbpBtbStats.commitBTBUnseenLoopBranchExitInLp++;
                }
            }
        }
    }

    for (auto component : components) {
        component->commitBranch(entry, inst);
    }
}

void
DecoupledBPUWithBTB::notifyInstCommit(const DynInstPtr &inst)
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

            // per phase BTB entries
            std::map<Addr, std::pair<BTBEntry, int>> currentPhaseBTBEntries;
            for (auto &it : totalBTBEntries) {
                auto &entry = it.second.first;
                auto visit_cnt = it.second.second;
                auto last_it = lastPhaseBTBEntries.find(it.first);
                if (last_it != lastPhaseBTBEntries.end()) {
                    visit_cnt -= last_it->second.second;
                }
                // use new entries, what if entry of the same start addr changes?
                if (visit_cnt > 0) {
                    currentPhaseBTBEntries[it.first] = std::make_pair(entry, visit_cnt);
                }
            }
            lastPhaseBTBEntries = totalBTBEntries;
            BTBEntriesByPhase.push_back(currentPhaseBTBEntries);

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
DecoupledBPUWithBTB::squashStreamAfter(unsigned squash_stream_id)
{
    auto erase_it = fetchStreamQueue.upper_bound(squash_stream_id);
    while (erase_it != fetchStreamQueue.end()) {
        DPRINTF(DecoupleBP || debugFlagOn || erase_it->second.startPC == ObservingPC,
                "Erasing stream %lu when squashing %d\n", erase_it->first,
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
                DPRINTF(LoopPredictorVerbose, "btb unseen loop entry %d: pc %#lx, endLoop %d, specCnt %d, tripCnty %d, conf %d\n",
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
DecoupledBPUWithBTB::dumpFsq(const char *when)
{
    DPRINTF(DecoupleBPProbe, "dumping fsq entries %s...\n", when);
    for (auto it = fetchStreamQueue.begin(); it != fetchStreamQueue.end();
         it++) {
        DPRINTFR(DecoupleBPProbe, "StreamID %lu, ", it->first);
        printStream(it->second);
    }
}



/**
 * @brief Attempts to enqueue a new entry into the Fetch Stream Queue (FSQ)
 * 
 * This function is called after a prediction has been generated and checks 
 * if the prediction can be enqueued into the FSQ. It will:
 * 1. Verify that a prediction is available
 * 2. Check if the PC is valid
 * 3. Wait for any override bubbles to be consumed
 * 4. Create a new FSQ entry with the prediction
 * 5. Clear prediction state for the next cycle
 */
void
DecoupledBPUWithBTB::tryEnqFetchStream()
{
    // 1. Check if a prediction is available to enqueue
    if (!receivedPred) {
        DPRINTF(Override, "No prediction available to enqueue into FSQ\n");
        return;
    }
    
    // 2. Validate PC value
    if (s0PC == MaxAddr) {
        DPRINTF(DecoupleBP, "Invalid PC value %#lx, cannot make prediction\n", s0PC);
        return;
    }
    
    // 3. Check for override bubbles
    // When higher stages override lower stages, bubbles are needed for pipeline consistency
    if (numOverrideBubbles > 0) {
        DPRINTF(Override, "Waiting for %u override bubbles before enqueuing\n", numOverrideBubbles);
        return;
    }
    
    // Ensure FSQ has space for the new entry
    assert(!streamQueueFull());
    
    // 4. Create new FSQ entry with current prediction
    makeNewPrediction(true);

    // 5. Reset prediction state for next cycle
    for (int i = 0; i < numStages; i++) {
        predsOfEachStage[i].btbEntries.clear();
    }
    
    receivedPred = false;
    DPRINTF(Override, "FSQ entry enqueued, prediction state reset\n");
}

void
DecoupledBPUWithBTB::setTakenEntryWithStream(const FetchStream &stream_entry, FtqEntry &ftq_entry)
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
DecoupledBPUWithBTB::setNTEntryWithStream(FtqEntry &ftq_entry, Addr end_pc)
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
DecoupledBPUWithBTB::tryEnqFetchTarget()
{
    DPRINTF(DecoupleBP, "Attempting to enqueue fetch target into FTQ\n");

    // 1. Check if FTQ can accept new entries
    if (fetchTargetQueue.full()) {
        DPRINTF(DecoupleBP, "Cannot enqueue - FTQ is full\n");
        return;
    }

    // 2. Check if FSQ has valid entries
    if (fetchStreamQueue.empty()) {
        dbpBtbStats.fsqNotValid++;
        DPRINTF(DecoupleBP, "Cannot enqueue - FSQ is empty\n");
        return;
    }

    // 3. Get FTQ enqueue state and find corresponding stream
    auto &ftq_enq_state = fetchTargetQueue.getEnqState();
    auto streamIt = fetchStreamQueue.find(ftq_enq_state.streamId);
    
    if (streamIt == fetchStreamQueue.end()) {
        dbpBtbStats.fsqNotValid++;
        DPRINTF(DecoupleBP, "Cannot enqueue - Stream ID %lu not found in FSQ\n",
                ftq_enq_state.streamId);
        return;
    }

    // 4. Get fetch stream and verify addresses
    auto &stream_to_enq = streamIt->second;
    Addr streamEndPC = stream_to_enq.predEndPC;
    
    DPRINTF(DecoupleBP, "Processing stream %lu (PC: %#lx)\n",
            streamIt->first, ftq_enq_state.pc);
    printStream(stream_to_enq);

    // Validation check - warn if FTQ enqueue PC is beyond FSQ end
    if (ftq_enq_state.pc > streamEndPC) {
        warn("Warning: FTQ enqueue PC %#lx is beyond FSQ end %#lx\n",
             ftq_enq_state.pc, streamEndPC);
    }

    // 5. Create and initialize new FTQ entry
    FtqEntry ftq_entry;
    ftq_entry.startPC = ftq_enq_state.pc;
    ftq_entry.fsqID = ftq_enq_state.streamId;

    // 6. Calculate FTQ entry boundaries
    Addr entryEndPC = streamEndPC;
    bool taken = stream_to_enq.getTaken();
    bool inLoop = stream_to_enq.fromLoopBuffer;
    bool loopExit = stream_to_enq.isExit;
    Addr loopEndPC = stream_to_enq.getBranchInfo().getEnd();
    
    // 7. Handle Jump-Ahead Predictor if enabled
    if (enableJumpAheadPredictor && stream_to_enq.jaHit) {
        // For jump-ahead prediction, divide stream into blocks
        int &currentSentBlock = stream_to_enq.currentSentBlock;
        entryEndPC = stream_to_enq.startPC + (currentSentBlock + 1) * predictWidth;
        currentSentBlock++;
    }

    // 8. Configure FTQ entry based on prediction (taken/not-taken)
    if (taken) {
        setTakenEntryWithStream(stream_to_enq, ftq_entry);
    } else {
        setNTEntryWithStream(ftq_entry, entryEndPC);
    }

    // 9. Update FTQ enqueue state for next entry
    // For loops: next PC depends on if we're exiting the loop
    // For non-loops: next PC depends on branch outcome
    if (inLoop) {
        ftq_enq_state.pc = loopExit ? loopEndPC : stream_to_enq.getBranchInfo().target;
    } else {
        ftq_enq_state.pc = taken ? stream_to_enq.getBranchInfo().target : entryEndPC;
    }
    
    // 10. Advance to next stream unless we're still processing jump-ahead blocks
    bool stillProcessingJABlocks = enableJumpAheadPredictor && 
                                  stream_to_enq.jaHit && 
                                  stream_to_enq.currentSentBlock < stream_to_enq.jaEntry.jumpAheadBlockNum;
    
    if (!stillProcessingJABlocks) {
        ftq_enq_state.streamId++;
    }
    
    DPRINTF(DecoupleBP, "Updated FTQ state: PC=%#lx, next stream ID=%lu\n",
            ftq_enq_state.pc, ftq_enq_state.streamId);

    // 11. Enqueue the entry and verify state
    fetchTargetQueue.enqueue(ftq_entry);
    assert(ftq_enq_state.streamId <= fsqId + 1);

    // 12. Debug output
    printFetchTarget(ftq_entry, "Insert to FTQ");
    fetchTargetQueue.dump("After insert new entry");
}

void
DecoupledBPUWithBTB::histShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history)
{
    if (shamt == 0) {
        return;
    }
    history <<= shamt;
    history[0] = taken;
}


// this function enqueues fsq and update s0PC and s0History
// use loop predictor and loop buffer here
void
DecoupledBPUWithBTB::makeNewPrediction(bool create_new_stream)
{
    DPRINTF(DecoupleBP, "Creating new prediction for PC %#lx\n", s0PC);

    // Create a new fetch stream entry
    FetchStream entry;
    entry.startPC = s0PC;
    
    // Initialize loop prediction info containers
    bool endLoop, isDouble, loopConf;
    std::vector<LoopRedirectInfo> lpRedirectInfos(numBr);
    std::vector<bool> fixNotExits(numBr);
    std::vector<LoopRedirectInfo> unseenLpRedirectInfos;
    
    // Normal prediction path (when loop buffer is not active)
    if (!enableLoopBuffer || (enableLoopBuffer && !lb.isActive())) {
        // 1. Initialize stream entry with default non-loop values
        entry.fromLoopBuffer = false;
        entry.isDouble = false;
        entry.isExit = false;

        // 2. Extract branch prediction information
        bool taken = finalPred.isTaken();
        Addr fallThroughAddr = finalPred.getFallThrough();
        Addr nextPC = finalPred.getTarget();
        
        // 3. Configure stream entry with prediction details
        entry.isHit = !finalPred.btbEntries.empty(); // TODO: fix isHit and falseHit
        entry.falseHit = false;
        entry.predBTBEntries = finalPred.btbEntries;
        entry.predTaken = taken;
        entry.predEndPC = fallThroughAddr;
        
        // 4. Set branch info for taken predictions
        if (taken) {
            entry.predBranchInfo = finalPred.getTakenEntry().getBranchInfo();
            entry.predBranchInfo.target = nextPC; // Use final target (may not be from BTB)
        }
        
        // 5. Update global PC state to target or fall-through
        s0PC = nextPC;

        // 6. Process jump-ahead predictor if enabled
        if (enableJumpAheadPredictor) {
            // Look up jump-ahead prediction for current PC
            bool jaHit, jaConf;
            JAEntry jaEntry;
            Addr jaTarget;
            std::tie(jaHit, jaConf, jaEntry, jaTarget) = jap.lookup(entry.startPC);
            // Jump-ahead code disabled for now - see TODO comment
        }

        // 7. Record current history and prediction metadata
        entry.history = s0History;
        entry.phrb = s0phrb;
        entry.phrt = s0phrt;
        entry.predTick = finalPred.predTick;
        entry.predSource = finalPred.predSource;

        // update phr
        if (taken) {
            updatePhr(s0PC, finalPred.getTarget());
        }

        // update (folded) histories for components
        for (int i = 0; i < numComponents; i++) {
            components[i]->specUpdateHist(s0History, finalPred);
            entry.predMetas[i] = components[i]->getPredictionMeta();
        }
        
        // 9. Update global history with new prediction
        int shamt;
        std::tie(shamt, taken) = finalPred.getHistInfo();
        histShiftIn(shamt, taken, s0History);
        
        // 10. Update history manager and verify TAGE folded history
        historyManager.addSpeculativeHist(
            entry.startPC, shamt, taken, entry.predBranchInfo, fsqId);
        tage->checkFoldedHist(s0History, "speculative update");
        
        // 11. Initialize default resolution state
        entry.setDefaultResolve();
    }
    // Else: loop buffer active path - currently no action needed
    
    // 12. Update loop-related information
    entry.loopRedirectInfos = lpRedirectInfos;
    entry.fixNotExits = fixNotExits;
    entry.unseenLoopRedirectInfos = unseenLpRedirectInfos;

    // 13. Insert the new stream entry into the fetch stream queue
    DPRINTF(LoopBuffer, "previous stream before loop:\n");
    printStream(lb.streamBeforeLoop);
    if (enableLoopBuffer && !lb.isActive()) {
        lb.recordNewestStreamOutsideLoop(entry);
    }
    DPRINTF(LoopBuffer, "now stream before loop:\n");
    printStream(lb.streamBeforeLoop);

    // if there are ahead pipelined predictors, get prevoius PCs
    unsigned max_ahead_pipeline_stages = 0;
    for (int i = 0; i < numComponents; i++) {
        max_ahead_pipeline_stages = std::max(max_ahead_pipeline_stages, components[i]->aheadPipelinedStages);
    }
    // get number of max_ahead_pipeline_stages previous PCs from fetchStreamQueue
    if (max_ahead_pipeline_stages > 0) {
        for (int i = 0; i < max_ahead_pipeline_stages; i++) {
            auto it = fetchStreamQueue.find(fsqId - max_ahead_pipeline_stages + i);
            if (it != fetchStreamQueue.end()) {
                // FIXME: it may not work well with jump ahead predictor
                entry.previousPCs.push(it->second.getRealStartPC());
            }
        }
    }
    auto [insertIt, inserted] = fetchStreamQueue.emplace(fsqId, entry);
    assert(inserted);

    // 14. Debug output and statistics
    dumpFsq("after insert new stream");
    DPRINTF(DecoupleBP, "Inserted fetch stream %lu starting at PC %#lx\n", 
            fsqId, entry.startPC);
    
    // 15. Update FSQ ID and increment statistics
    fsqId++;
    printStream(entry);
    dbpBtbStats.fsqEntryEnqueued++;
}

void
DecoupledBPUWithBTB::checkHistory(const boost::dynamic_bitset<> &history)
{
    unsigned ideal_size = 0;
    boost::dynamic_bitset<> ideal_hash_hist(historyBits, 0);
    for (const auto entry: historyManager.getSpeculativeHist()) {
        if (entry.shamt != 0) {
            ideal_size += entry.shamt;
            DPRINTF(DecoupleBPVerbose, "pc: %#lx, shamt: %lu, cond_taken: %d\n", entry.pc,
                    entry.shamt, entry.cond_taken);
            ideal_hash_hist <<= entry.shamt;
            ideal_hash_hist[0] = entry.cond_taken;
        }
    }
    unsigned comparable_size = std::min(ideal_size, historyBits);
    boost::dynamic_bitset<> sized_real_hist(history);
    ideal_hash_hist.resize(comparable_size);
    sized_real_hist.resize(comparable_size);

    // boost::to_string(ideal_hash_hist, buf1);
    // boost::to_string(sized_real_hist, buf2);
    DPRINTF(DecoupleBP,
            "Ideal size:\t%u, real history size:\t%u, comparable size:\t%u\n",
            ideal_size, historyBits, comparable_size);
    // DPRINTF(DecoupleBP, "Ideal history:\t%s\nreal history:\t%s\n",
    //         buf1.c_str(), buf2.c_str());
    assert(ideal_hash_hist == sized_real_hist);
}

void
DecoupledBPUWithBTB::resetPC(Addr new_pc)
{
    s0PC = new_pc;
    fetchTargetQueue.resetPC(new_pc);
}

Addr
DecoupledBPUWithBTB::getPreservedReturnAddr(const DynInstPtr &dynInst)
{
    DPRINTF(DecoupleBP, "acquiring reutrn address for inst pc %#lx from decode\n", dynInst->pcState().instAddr());
    auto fsqid = dynInst->getFsqId();
    auto it = fetchStreamQueue.find(fsqid);
    auto retAddr = ras->getTopAddrFromMetas(it->second);
    DPRINTF(DecoupleBP, "get ret addr %#lx\n", retAddr);
    return retAddr;
}

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
