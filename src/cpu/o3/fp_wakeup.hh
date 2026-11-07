/* Copyright (c) 2026. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __CPU_O3_FP_WAKEUP_HH__
#define __CPU_O3_FP_WAKEUP_HH__

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gem5
{
namespace o3
{

/**
 * Identity of one scheduling attempt, captured by events when they are created.
 * A retry changes attempt; the saved identity of an earlier event stays intact.
 */
struct FpWakeIdentity
{
    uint32_t tid = 0; ///< Hardware thread that owns the instruction.
    uint64_t seq = 0; ///< Dynamic instruction sequence number.
    uint64_t attempt = 0; ///< Scheduling generation, advanced on cancellation.

    /** Compare the complete attempt identity, including the thread. */
    bool operator==(const FpWakeIdentity &other) const
    { return tid == other.tid && seq == other.seq && attempt == other.attempt; }
};

/** Producer classes eligible to publish FP result promises. */
enum class FpProducerKind { Fmul, Fma };
/** Wake paths two/four CPU cycles before expected data in the scheduler. */
enum class FpWakePath { M2, M4 };
/**
 * Operand check outcomes: wait, real data available, unusable binding, overdue
 * data, or an architectural fault awaiting the normal recovery path.
 */
enum class FpDataStatus
{
    Pending, Ready, Canceled, MissedDeadline, FaultPending
};

struct SourceBinding;
/**
 * A producer's result promise and its dependency edges for one attempt.
 * Time fields use the caller's unit (absolute Ticks in Scheduler). The flags
 * record distinct facts; publication/confirmation never fabricate real data.
 */
struct WakeToken
{
    FpWakeIdentity producer; ///< Immutable identity used by queued events.
    FpProducerKind kind = FpProducerKind::Fmul; ///< M4 priority class.
    unsigned outputChannel = 0; ///< Producer output-port identifier.
    unsigned m4Endpoint = 0; ///< Shared endpoint used for M4 arbitration.
    uint64_t m2Cycle = 0; ///< Scheduled M2 confirmation/broadcast time.
    uint64_t m4Cycle = 0; ///< Scheduled M4 early-addend wake time.
    uint64_t dataCycle = 0; ///< Expected real-result time, not readiness.
    bool admitted = false; ///< Passed FP admission before IQ release.
    bool confirmed = false; ///< Passed confirmation at or after M2.
    bool dataReady = false; ///< Native execution produced a valid result.
    bool canceled = false; ///< Permanently revoked; retries use new tokens.
    bool published = false; ///< Arbitration succeeded; wakes may be queued.
    /** M2 broadcast recorded; late users may inherit it. */
    bool m2Broadcast = false;
    bool faulted = false; ///< This dependency chain has a pending fault.
    std::vector<uint64_t> registers; ///< Destinations to clear if still owned.
    /** Consumer edges are weak to avoid cycles with bindings owning tokens. */
    std::vector<std::weak_ptr<SourceBinding>> users;
    /** Upstream bindings checked again at admission and confirmation. */
    std::vector<std::weak_ptr<SourceBinding>> inputs;
};

/**
 * One consumer source bound to one producer attempt and wake path.
 * Aliased physical sources keep separate bindings, allowing an FMA addend to
 * use M4 while a multiplicand uses M2. ready means scheduling eligibility.
 */
struct SourceBinding
{
    FpWakeIdentity consumer; ///< Consumer attempt owning this source.
    unsigned source = 0; ///< Source operand index within the instruction.
    std::shared_ptr<WakeToken> token; ///< Keeps the producer result alive.
    FpWakePath path = FpWakePath::M2; ///< Ordinary or late-addend read path.
    bool ready = false; ///< Woken for scheduling; real data may be pending.
    bool selected = false; ///< Selection has fixed the read deadline.
    bool canceled = false; ///< Binding revoked; retries create fresh bindings.
    uint64_t deadline = 0; ///< Real-data deadline in the caller's time unit.
};

/**
 * FP promise and source-binding state shared by Scheduler and unit tests.
 *
 * Callers drive all transitions, event scheduling and native execution. This
 * component never reads register values or schedules simulator events. Time
 * arguments must share one unit; Scheduler supplies absolute Ticks and scales
 * CPU-cycle delays by clockPeriod(). See
 * docs/design-docs/backend/fma_wakeup_design.md for call order.
 */
class FpWakeupState
{
  public:
    using Token = std::shared_ptr<WakeToken>;
    using Binding = std::shared_ptr<SourceBinding>;

    /**
     * Create an unpublished token and remember it as the latest attempt.
     * Reject duplicate or non-increasing live attempt identities. A newer
     * attempt cancels its predecessor and the predecessor's token dependents.
     */
    Token prepare(FpWakeIdentity producer, FpProducerKind kind,
                  unsigned output_channel, unsigned m4_endpoint,
                  uint64_t m2_cycle, uint64_t m4_cycle,
                  uint64_t data_cycle);

    /**
     * Look up the exact identity, including canceled entries; null if absent.
     */
    Token find(FpWakeIdentity producer) const;

    /**
     * Test latest-attempt ownership and non-cancellation.
     * This does not check publication, faults, confirmation or real data.
     */
    bool current(const Token &token) const;

    /**
     * Mark a current token published; return false for a non-current token.
     */
    bool publish(FpWakeIdentity producer);

    /**
     * Record admission after validateAdmission succeeds.
     * This does not release the IQ entry or require real input data.
     */
    bool admit(FpWakeIdentity producer);

    /**
     * Confirm a current, admitted token at or after its M2 time.
     * Revalidate upstream bindings because inputs may change after admission.
     */
    bool confirm(FpWakeIdentity producer, uint64_t cycle);

    /**
     * Mark real data ready for a current, admitted, non-faulted producer.
     * The caller establishes native completion; this does not check time or
     * require confirmed, and must not be used to fulfill a predicted result.
     */
    bool complete(FpWakeIdentity producer);

    /**
     * Mark a current producer and its token dependents as fault pending.
     * Do not fabricate data or perform a squash; precise-trap recovery is
     * owned by the pipeline.
     */
    void fault(FpWakeIdentity producer);

    /**
     * Create an independent source binding and register it with the producer.
     * A published current token is ready for M4, or for M2 after its broadcast.
     * A reclaimed confirmed, completed result can also be rebound if it has
     * no replacement in latest; this does not restore register ownership.
     */
    Binding bind(FpWakeIdentity consumer, unsigned source, const Token &token,
                 FpWakePath path);

    /**
     * Attach an upstream binding to a current producer for revalidation.
     * Duplicate edges are ignored. A missing binding or stale producer
     * is invalid.
     */
    void addInput(FpWakeIdentity producer, const Binding &binding);

    /**
     * Fix the data deadline on first selection of a usable binding.
     * M2 reads at at_fu; M4 reads at at_fu + late_read_delay. Repeated calls
     * preserve the original deadline. All arguments use the caller's unit.
     */
    bool select(const Binding &binding, uint64_t at_fu,
                uint64_t late_read_delay = 2);

    /**
     * Require a current non-faulted producer and live, usable input bindings
     * whose producers are confirmed and non-faulted. Real data may be pending.
     */
    bool validateAdmission(FpWakeIdentity producer) const;

    /**
     * Classify the input: unusable, fault pending, ready, overdue, or waiting.
     * Faults take precedence over data/deadline checks. No state is changed.
     */
    static FpDataStatus checkData(const Binding &binding, uint64_t cycle);

    /**
     * Test existence, non-cancellation and source scheduling readiness only.
     * Fault, confirmation and real-data checks belong to the caller's phase.
     */
    static bool usable(const Binding &binding);

    /**
     * Confirm the producer, then mark M2 broadcast and wake existing users.
     * Unselected, uncanceled bindings switch to ordinary M2 reads; selected
     * bindings retain their path/deadline. Return false if confirmation fails.
     */
    bool broadcastM2(FpWakeIdentity producer, uint64_t cycle);

    /**
     * Associate a physical destination with a current token, replacing any
     * previous mapping, and remember the destination for ownership cleanup.
     */
    void setRegister(uint64_t reg, const Token &token);

    /**
     * Find a current, non-faulted owner with confirmed and broadcast M2.
     * Returns null when a late consumer cannot inherit that ordinary wake.
     */
    Token inheritM2(uint64_t reg) const;

    /**
     * Remove the register mapping only if expected still owns it.
     */
    void clearRegister(uint64_t reg, const Token &expected);

    /**
     * Revoke a token and transitively revoke its token-dependent producers.
     * Invalidate affected source bindings and return newly canceled identities.
     * This does not erase indexes or cancel ordinary scheduler dependencies.
     */
    std::vector<FpWakeIdentity> cancel(FpWakeIdentity producer);

    /**
     * Remove one token's indexes and any register mappings it still owns.
     * This does not cancel the token; consumer-held completed results survive.
     */
    void erase(FpWakeIdentity producer);

    /**
     * Cancel and erase tokens younger than after_seq in the selected thread.
     * This state-only helper does not flush pipeline structures. Production
     * IQ squash calls Scheduler cancellation and reclamation per instruction.
     */
    void squash(uint32_t tid, uint64_t after_seq);

    /**
     * Cancel all tokens and clear indexes, invalidating outstanding bindings.
     * The peak-size statistic is retained.
     */
    void clear();

    /**
     * Return the indexed-token count, including canceled tokens not erased.
     */
    std::size_t size() const { return tokens.size(); }

    /**
     * Return the largest indexed-token count seen by this state object.
     */
    std::size_t peakSize() const { return peak; }

    /**
     * Choose one published, uncanceled token due at cycle per M4 endpoint.
     * FMA wins over FMUL; within a kind, the older sequence wins. Callers
     * filter obsolete/faulted instructions before supplying candidates.
     */
    static std::vector<Token> arbitrateM4(const std::vector<Token> &candidates,
                                        uint64_t cycle);

  private:
    /** Hash all three identity fields used by exact-attempt lookups. */
    struct Hash
    {
        std::size_t operator()(const FpWakeIdentity &id) const;
    };

    /** Exact-attempt ownership; erase() releases the owning reference. */
    std::unordered_map<FpWakeIdentity, Token, Hash> tokens;
    /** Latest token per (tid, seq), with attempt normalized to zero. */
    std::unordered_map<FpWakeIdentity, std::weak_ptr<WakeToken>, Hash> latest;
    /** Physical-register owners used by late M2 inheritance. */
    std::unordered_map<uint64_t, std::weak_ptr<WakeToken>> registers;
    std::size_t peak = 0;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_FP_WAKEUP_HH__
