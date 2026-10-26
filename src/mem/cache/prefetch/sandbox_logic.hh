/*
 * Sandbox Prefetching (Pugsley et al., HPCA 2014).
 * The evaluation state is independent of the gem5 prefetch queue.
 */

#ifndef __MEM_CACHE_PREFETCH_SANDBOX_LOGIC_HH__
#define __MEM_CACHE_PREFETCH_SANDBOX_LOGIC_HH__

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace gem5
{
namespace prefetch
{

class SandboxLogic
{
  public:
    struct Config
    {
        unsigned degree = 4;
        bool detectStreams = true;
        unsigned streamLength = 3;
        unsigned accessesPerPhase = 256;
        unsigned cycleOffsets = 4;
        unsigned bloomBits = 2048;
        uint64_t seed = 300;
        unsigned blockBytes = 64;
        unsigned pageBytes = 4096;
    };

    struct Prediction
    {
        uint64_t address;
        int offset;
        unsigned lookahead;
    };

    struct Result
    {
        bool bloomHit = false;
        bool phaseEnd = false;
        bool roundEnd = false;
        std::vector<Prediction> predictions;
    };

    explicit SandboxLogic(const Config &config);
    Result access(uint64_t address);

    int evaluatingOffset() const { return candidates[active].offset; }
    unsigned phaseAccesses() const { return phaseAccessCount; }
    unsigned scoreFor(int offset) const;
    bool completedFor(int offset) const;
    std::vector<int> evaluatedOffsets() const;
    unsigned bloomHashes() const { return hashCount; }

  private:
    struct Candidate
    {
        int offset;
        unsigned score = 0;
        bool completed = false;
    };

    static uint64_t mix(uint64_t x);
    std::optional<uint64_t> target(uint64_t address, int offset,
                                   unsigned lookahead) const;
    bool bloomContains(uint64_t address) const;
    void bloomAdd(uint64_t address);
    void beginPhase();
    void endRound();
    void generate(uint64_t address, bool positive,
                  std::vector<Prediction> &predictions) const;

    const Config cfg;
    const unsigned hashCount;
    std::vector<uint64_t> bloom;
    std::vector<Candidate> candidates;
    std::deque<int> idleOffsets;
    unsigned active = 0;
    unsigned phaseAccessCount = 0;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_SANDBOX_LOGIC_HH__
