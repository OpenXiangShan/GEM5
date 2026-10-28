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

/** Identity captured by every event, rather than read from a retried inst. */
struct FpWakeIdentity
{
    uint32_t tid = 0;
    uint64_t seq = 0;
    uint64_t attempt = 0;
    bool operator==(const FpWakeIdentity &other) const
    { return tid == other.tid && seq == other.seq && attempt == other.attempt; }
};

enum class FpProducerKind { Fmul, Fma };
enum class FpWakePath { M2, M4 };
enum class FpDataStatus
{
    Pending, Ready, Canceled, MissedDeadline, FaultPending
};

struct SourceBinding;
struct WakeToken
{
    FpWakeIdentity producer;
    FpProducerKind kind = FpProducerKind::Fmul;
    unsigned outputChannel = 0;
    unsigned m4Endpoint = 0;
    uint64_t m2Cycle = 0;
    uint64_t m4Cycle = 0;
    uint64_t dataCycle = 0;
    bool admitted = false;
    bool confirmed = false;
    bool dataReady = false;
    bool canceled = false;
    bool published = false;
    bool m2Broadcast = false;
    bool faulted = false;
    std::vector<uint64_t> registers;
    std::vector<std::weak_ptr<SourceBinding>> users;
    std::vector<std::weak_ptr<SourceBinding>> inputs;
};

/** Each source has its own binding, even when physical registers alias. */
struct SourceBinding
{
    FpWakeIdentity consumer;
    unsigned source = 0;
    std::shared_ptr<WakeToken> token;
    FpWakePath path = FpWakePath::M2;
    bool ready = false;
    bool selected = false;
    bool canceled = false;
    uint64_t deadline = 0;
};

/** Pure state shared by the scheduler and unit tests. No scheduled events. */
class FpWakeupState
{
  public:
    using Token = std::shared_ptr<WakeToken>;
    using Binding = std::shared_ptr<SourceBinding>;

    Token prepare(FpWakeIdentity producer, FpProducerKind kind,
                  unsigned output_channel, unsigned m4_endpoint,
                  uint64_t m2_cycle, uint64_t m4_cycle,
                  uint64_t data_cycle);
    Token find(FpWakeIdentity producer) const;
    bool current(const Token &token) const;
    bool publish(FpWakeIdentity producer);
    bool admit(FpWakeIdentity producer);
    bool confirm(FpWakeIdentity producer, uint64_t cycle);
    bool complete(FpWakeIdentity producer);
    /** Preserve architectural faults until baseline trap recovery. */
    void fault(FpWakeIdentity producer);

    Binding bind(FpWakeIdentity consumer, unsigned source, const Token &token,
                 FpWakePath path);
    void addInput(FpWakeIdentity producer, const Binding &binding);
    /** All times and the late-read delay use the caller's time unit. */
    bool select(const Binding &binding, uint64_t at_fu,
                uint64_t late_read_delay = 2);
    bool validateAdmission(FpWakeIdentity producer) const;
    static FpDataStatus checkData(const Binding &binding, uint64_t cycle);
    static bool usable(const Binding &binding);
    /** M2 converts unselected M4 users to ordinary operand reads. */
    bool broadcastM2(FpWakeIdentity producer, uint64_t cycle);

    void setRegister(uint64_t reg, const Token &token);
    Token inheritM2(uint64_t reg) const;
    void clearRegister(uint64_t reg, const Token &expected);
    std::vector<FpWakeIdentity> cancel(FpWakeIdentity producer);
    void erase(FpWakeIdentity producer);
    void squash(uint32_t tid, uint64_t after_seq);
    void clear();
    std::size_t size() const { return tokens.size(); }
    std::size_t peakSize() const { return peak; }
    /** One winner per endpoint: FMA before FMUL, then older sequence. */
    static std::vector<Token> arbitrateM4(const std::vector<Token> &candidates,
                                        uint64_t cycle);

  private:
    struct Hash
    {
        std::size_t operator()(const FpWakeIdentity &id) const;
    };
    std::unordered_map<FpWakeIdentity, Token, Hash> tokens;
    std::unordered_map<FpWakeIdentity, std::weak_ptr<WakeToken>, Hash> latest;
    std::unordered_map<uint64_t, std::weak_ptr<WakeToken>> registers;
    std::size_t peak = 0;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_FP_WAKEUP_HH__
