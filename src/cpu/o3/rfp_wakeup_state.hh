/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of
 * Sciences
 * All rights reserved.
 *
 * The license is the same as that in register_prefetcher.hh.
 */

#ifndef __CPU_O3_RFP_WAKEUP_STATE_HH__
#define __CPU_O3_RFP_WAKEUP_STATE_HH__

namespace gem5
{
namespace o3
{

/**
 * Tracks the issue-stage lifetime of an RFP dependent wakeup.
 *
 * The producer's issue-stage-0 event is the only event allowed to wake
 * consumers. A cache response records data readiness but never sends a wakeup;
 * the consumer's final issue gate separately requires both the response data
 * and S0 validation. If the producer cannot leave the issue pipeline, its
 * speculative wakeup is retracted and a later issue attempt may wake again.
 */
class RfpWakeupState
{
  public:
    enum class Action
    {
        None,
        Wake,
        Retract
    };

    Action
    onProducerIssue()
    {
        if (producerIssueActive) {
            return Action::None;
        }
        producerIssueActive = true;
        wakeSent = true;
        return Action::Wake;
    }

    Action
    onDataReady()
    {
        dataReady = true;
        return Action::None;
    }

    Action
    onProducerIssueCanceled()
    {
        producerIssueActive = false;
        if (!wakeSent) {
            return Action::None;
        }
        wakeSent = false;
        return Action::Retract;
    }

    bool producerIssued() const { return producerIssueActive; }
    bool hasData() const { return dataReady; }
    bool woken() const { return wakeSent; }

  private:
    bool producerIssueActive = false;
    bool dataReady = false;
    bool wakeSent = false;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_RFP_WAKEUP_STATE_HH__
