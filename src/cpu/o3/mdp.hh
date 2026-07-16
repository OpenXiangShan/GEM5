#ifndef __CPU_O3_MDP_HH__
#define __CPU_O3_MDP_HH__

namespace gem5
{
namespace o3
{

enum class MDPFeedbackSource
{
    NoForward,
    StoreQueue,
    StoreBuffer,
};

enum class MDPPredictionOutcome
{
    TargetedNoReplayNoSq,
    TargetedNoReplaySq,
    TargetedReplayedNoSq,
    TargetedReplayedSq,
    WaitAllNoReplayNoSq,
    WaitAllNoReplaySq,
    WaitAllReplayedNoSq,
    WaitAllReplayedSq,
    NumOutcomes,
};

constexpr unsigned NumMDPPredictionOutcomes =
    static_cast<unsigned>(MDPPredictionOutcome::NumOutcomes);

constexpr MDPPredictionOutcome
mdpPredictionOutcome(bool wait_all, bool replayed, bool sq_forwarded)
{
    const auto outcome = (wait_all ? 4U : 0U) |
                         (replayed ? 2U : 0U) |
                         (sq_forwarded ? 1U : 0U);
    return static_cast<MDPPredictionOutcome>(outcome);
}

constexpr const char *
mdpPredictionOutcomeName(MDPPredictionOutcome outcome)
{
    switch (outcome) {
      case MDPPredictionOutcome::TargetedNoReplayNoSq:
        return "targeted_no_replay_no_sq";
      case MDPPredictionOutcome::TargetedNoReplaySq:
        return "targeted_no_replay_sq";
      case MDPPredictionOutcome::TargetedReplayedNoSq:
        return "targeted_replayed_no_sq";
      case MDPPredictionOutcome::TargetedReplayedSq:
        return "targeted_replayed_sq";
      case MDPPredictionOutcome::WaitAllNoReplayNoSq:
        return "wait_all_no_replay_no_sq";
      case MDPPredictionOutcome::WaitAllNoReplaySq:
        return "wait_all_no_replay_sq";
      case MDPPredictionOutcome::WaitAllReplayedNoSq:
        return "wait_all_replayed_no_sq";
      case MDPPredictionOutcome::WaitAllReplayedSq:
        return "wait_all_replayed_sq";
      case MDPPredictionOutcome::NumOutcomes:
        return "invalid";
    }

    return "invalid";
}

/** Record at most one MDP prediction-accuracy outcome per dynamic load. */
template <typename Outcomes>
bool
recordMDPPredictionOutcome(Outcomes &outcomes, bool &updated,
                           bool wait_all, bool replayed, bool sq_forwarded)
{
    if (updated) {
        return false;
    }

    ++outcomes[static_cast<unsigned>(
        mdpPredictionOutcome(wait_all, replayed, sq_forwarded))];
    updated = true;
    return true;
}

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MDP_HH__
