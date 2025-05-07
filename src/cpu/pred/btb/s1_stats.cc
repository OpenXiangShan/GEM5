#include "cpu/pred/btb/s1_stats.hh"

#include "cpu/pred/btb/btb.hh"
#include "cpu/pred/btb/stream_struct.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{
void
S1StatsHelper::updateIndirectCauseForTakenBranch(
    const FetchStream &stream, const FullBTBPrediction &fullPred,
    MispredictionIndirectCause &indirectCause) {
    if (stream.getBranchInfo().isCond){
        // check whether the conditional branch is "hit but NT" or "miss" during prediction
        auto it = fullPred.condTakens.find(stream.getControlPC());
        if (it != fullPred.condTakens.end()) {
            assert(!it->second);
            indirectCause.condTButPredNT++;
        } else {
            indirectCause.condTButPredMiss++;
        }
    } else {
        // uncond miss at prediction
        indirectCause.uncondMiss++;
    }
}

void S1StatsHelper::updateIndirectCauseForNotTakenBranch(
    FullBTBPrediction &fullPred, MispredictionIndirectCause &indirectCause) {
    // count the instance where the taken Entry of the prediction is not taken in execution
    const auto &takenEntry = fullPred.getTakenEntry();
    auto it = fullPred.condTakens.find(takenEntry.pc);
    if (takenEntry.isCond &&
        it != fullPred.condTakens.end() &&
        it->second)
    {
        indirectCause.condNTButPredT++;
    } else {
        indirectCause.others++;
    }
}

void S1StatsHelper::countMispredictionCause
    (const FetchStream &stream, FullBTBPrediction &fullPred,
    MispredictionDirectCause &directCause, MispredictionIndirectCause &indirectCause) {
    if (stream.getTaken() != fullPred.isTaken()) {
        // fall through misprediction
        directCause.misPredCount++;
        directCause.fallThrough++;

        // check and increment the first mispred branch
        if (stream.getTaken()) {
            updateIndirectCauseForTakenBranch(stream, fullPred, indirectCause);
        } else {
            // exe fall through, but predict taken, almost always a cond direction misprediction
            // assert(fullPred.getTakenEntry().isCond); // too bold an assertion?
            updateIndirectCauseForNotTakenBranch(fullPred, indirectCause);
        }
    }else if (stream.getTaken() && fullPred.isTaken()){
        // both predict taken and exe taken

        if (stream.getControlPC() != fullPred.getTakenEntry().pc) {
            directCause.misPredCount++;
            directCause.controlAddr++;

            // check and increment the first mispred branch
            auto exeTakenPC = stream.getControlPC();
            auto predTakenPC = fullPred.getTakenEntry().pc;
            if (exeTakenPC < predTakenPC){
                // the first mispred branch is before the taken one
                updateIndirectCauseForTakenBranch(stream, fullPred, indirectCause);
            } else {
                // the first mispred branch is after the taken one
                updateIndirectCauseForNotTakenBranch(fullPred, indirectCause);
            }
        } else if (stream.getTakenTarget() != fullPred.getTarget(predictWidth)){
            directCause.misPredCount++;
            directCause.target++;

            assert(stream.getBranchInfo().isIndirect);
            indirectCause.indirectTargetWrong++;
        } else {
            // no misprediction
        }
    } else {
        // predict fall through, so does execution
    }
    return;
}

FullBTBPrediction
S1StatsHelper::generateFullPred(const std::vector<BTBEntry> &entries, Addr startPC, bool forceTaken)
{
    FullBTBPrediction res = FullBTBPrediction();
    for (auto e : entries) {
        res.btbEntries.push_back(BTBEntry(e));
    }
    for (auto &e : entries) {
        assert(e.valid);
        if (e.isCond) {
            res.condTakens[e.pc] = e.alwaysTaken || (e.ctr >= 0) || forceTaken;
        } else if (e.isIndirect) {
            res.indirectTargets[e.pc] = e.target;
            if (e.isReturn) {
                res.returnTarget = e.target;
            }
            break;
        }
    }
    res.bbStart = startPC;
    return res;
}

void
S1StatsHelper::update(const FetchStream & stream) {
    assert(ubtb);
    assert(abtb);
    //assert(stream.resolved); // stream is only "resolved" when squash happens
    // in other words, "unresolved" stream always has the same exeBranchInfo as PredBranchInfo
    auto meta = std::static_pointer_cast<S1StatsMeta>(stream.predMetas[getComponentIdx()]);
    auto ubtbHitEntry = meta->ubtbMeta.hit_entry;
    auto abtbHitEntries = meta->abtbMeta.hit_entries;


    // track the source of S0 prediction
    if (ubtbHitEntry.valid) {
        s1Stats.S1PredUseUBTB++;
        auto ubtbFullPred = generateFullPred({ubtbHitEntry}, stream.startPC, true);
        //assert(ubtbFullPred.isTaken());// ubtb should always predict taken when it hit
        countMispredictionCause(stream, ubtbFullPred, ubtbMisPredDirectCause, ubtbMisPredIndirectCause);

    } else if (abtbHitEntries.size() > 0) {
        s1Stats.S1PredUseABTB++;
        auto abtbFullPred = generateFullPred(abtbHitEntries, stream.startPC);
        countMispredictionCause(stream, abtbFullPred, abtbMisPredDirectCause, abtbMisPredIndirectCause);
    } else {
        s1Stats.S1Predmiss++;
    }



}


S1StatsHelper::S1Stats::S1Stats(statistics::Group *parent)
    :   statistics::Group(parent),
        ADD_STAT(S1Predmiss, statistics::units::Count::get(),
                "misses encountered on S0 prediction, i.e. uBTB and ABTB miss"),
        ADD_STAT(S1PredUseUBTB, statistics::units::Count::get(), "uBTB prediction used, i.e. uBTB hit"),
        ADD_STAT(S1PredUseABTB, statistics::units::Count::get(),
                "aBTB prediction used, i.e. uBTB miss and ABTB hit")

{
}
S1StatsHelper::MispredictionDirectCause::MispredictionDirectCause(statistics::Group* parent, const char* name)
    : statistics::Group(parent, name),
      ADD_STAT(misPredCount, statistics::units::Count::get(), "Total mispredictions"),
      ADD_STAT(fallThrough, statistics::units::Count::get(), "Fall-through mispredictions"),
      ADD_STAT(controlAddr, statistics::units::Count::get(), "Control address mispredictions"),
      ADD_STAT(target, statistics::units::Count::get(), "Target mispredictions")
{
}

S1StatsHelper::MispredictionIndirectCause::MispredictionIndirectCause(statistics::Group* parent, const char* name)
    : statistics::Group(parent, name),
      ADD_STAT(condTButPredMiss, statistics::units::Count::get(), "Conditional taken but predicted miss"),
      ADD_STAT(condTButPredNT, statistics::units::Count::get(), "Conditional taken but predicted not taken"),
      ADD_STAT(condNTButPredT, statistics::units::Count::get(), "Conditional not taken but predicted taken"),
      ADD_STAT(uncondMiss, statistics::units::Count::get(), "Unconditional miss"),
      ADD_STAT(indirectTargetWrong, statistics::units::Count::get(), "Indirect target wrong"),
      ADD_STAT(others, statistics::units::Count::get(), "Other mispredictions")
{
}

}}}
