#include "mem/cache/prefetch/sandbox_logic.hh"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gem5
{
namespace prefetch
{

SandboxLogic::SandboxLogic(const Config &config)
    : cfg(config),
      hashCount(std::max(1u, static_cast<unsigned>(std::ceil(
          static_cast<double>(config.bloomBits) /
          std::max(1u, config.accessesPerPhase) *
          std::log(2.0))))),
      bloom((config.bloomBits + 63) / 64, 0)
{
    if (!cfg.degree || !cfg.accessesPerPhase || !cfg.bloomBits ||
        !cfg.blockBytes || !cfg.pageBytes ||
        cfg.pageBytes % cfg.blockBytes || cfg.pageBytes / cfg.blockBytes <= 16 ||
        cfg.cycleOffsets > 16 || cfg.streamLength > 3) {
        throw std::invalid_argument("Invalid Sandbox prefetcher parameters");
    }
    for (int offset = 1; offset <= 8; ++offset)
        candidates.push_back({offset});
    for (int offset = -8; offset <= -1; ++offset)
        candidates.push_back({offset});
    for (int offset = 9; offset <= 16; ++offset)
        idleOffsets.push_back(offset);
    for (int offset = -16; offset <= -9; ++offset)
        idleOffsets.push_back(offset);
}

uint64_t
SandboxLogic::mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

std::optional<uint64_t>
SandboxLogic::target(uint64_t address, int offset, unsigned lookahead) const
{
    const uint64_t page = address / cfg.pageBytes;
    const int64_t block = static_cast<int64_t>(
        (address % cfg.pageBytes) / cfg.blockBytes);
    const int64_t next = block +
        static_cast<int64_t>(offset) * lookahead;
    if (next < 0 || next >= cfg.pageBytes / cfg.blockBytes)
        return std::nullopt;
    return page * cfg.pageBytes + next * cfg.blockBytes;
}

bool
SandboxLogic::bloomContains(uint64_t address) const
{
    const uint64_t line = address / cfg.blockBytes;
    const uint64_t first = mix(line ^ cfg.seed);
    const uint64_t second = mix(line ^ (cfg.seed + 0x9e3779b97f4a7c15ULL)) | 1;
    for (unsigned i = 0; i < hashCount; ++i) {
        const uint64_t bit = (first + i * second) % cfg.bloomBits;
        if (!(bloom[bit / 64] & (uint64_t{1} << (bit % 64))))
            return false;
    }
    return true;
}

void
SandboxLogic::bloomAdd(uint64_t address)
{
    const uint64_t line = address / cfg.blockBytes;
    const uint64_t first = mix(line ^ cfg.seed);
    const uint64_t second = mix(line ^ (cfg.seed + 0x9e3779b97f4a7c15ULL)) | 1;
    for (unsigned i = 0; i < hashCount; ++i) {
        const uint64_t bit = (first + i * second) % cfg.bloomBits;
        bloom[bit / 64] |= uint64_t{1} << (bit % 64);
    }
}

void
SandboxLogic::beginPhase()
{
    std::fill(bloom.begin(), bloom.end(), 0);
    phaseAccessCount = 0;
    candidates[active].score = 0;
    candidates[active].completed = false;
}

void
SandboxLogic::endRound()
{
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b) {
                         return a.score > b.score;
                     });
    for (unsigned i = 0; i < cfg.cycleOffsets; ++i) {
        idleOffsets.push_back(candidates.back().offset);
        candidates.pop_back();
    }
    for (unsigned i = 0; i < cfg.cycleOffsets; ++i) {
        const int offset = idleOffsets.front();
        idleOffsets.pop_front();
        candidates.push_back({offset});
    }
}

void
SandboxLogic::generate(uint64_t address, bool positive,
                       std::vector<Prediction> &predictions) const
{
    std::vector<const Candidate *> sorted;
    for (const auto &candidate : candidates) {
        if (candidate.completed && (candidate.offset > 0) == positive)
            sorted.push_back(&candidate);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const Candidate *a, const Candidate *b) {
                  return std::abs(a->offset) < std::abs(b->offset);
              });

    unsigned count = 0;
    const unsigned maxLookahead = cfg.detectStreams ? cfg.streamLength : 1;
    for (const auto *candidate : sorted) {
        for (unsigned ahead = 1; ahead <= maxLookahead && count < cfg.degree;
             ++ahead) {
            if (candidate->score < ahead * cfg.accessesPerPhase)
                break;
            if (auto addressToPrefetch =
                    target(address, candidate->offset, ahead)) {
                predictions.push_back(
                    {*addressToPrefetch, candidate->offset, ahead});
                ++count;
            }
        }
        if (count == cfg.degree)
            break;
    }
}

SandboxLogic::Result
SandboxLogic::access(uint64_t address)
{
    Result result;
    auto &candidate = candidates[active];
    result.bloomHit = bloomContains(address);
    if (result.bloomHit) {
        ++candidate.score;
        if (cfg.detectStreams) {
            for (unsigned ahead = 1; ahead <= cfg.streamLength; ++ahead) {
                if (auto previous = target(address, -candidate.offset, ahead);
                    previous && bloomContains(*previous)) {
                    ++candidate.score;
                }
            }
        }
    }
    if (auto predicted = target(address, candidate.offset, 1))
        bloomAdd(*predicted);

    if (++phaseAccessCount == cfg.accessesPerPhase) {
        result.phaseEnd = true;
        candidate.completed = true;
        if (++active == candidates.size()) {
            result.roundEnd = true;
            endRound();
            active = 0;
        }
        beginPhase();
    }

    generate(address, true, result.predictions);
    generate(address, false, result.predictions);
    return result;
}

unsigned
SandboxLogic::scoreFor(int offset) const
{
    for (const auto &candidate : candidates)
        if (candidate.offset == offset)
            return candidate.score;
    return 0;
}

bool
SandboxLogic::completedFor(int offset) const
{
    for (const auto &candidate : candidates)
        if (candidate.offset == offset)
            return candidate.completed;
    return false;
}

std::vector<int>
SandboxLogic::evaluatedOffsets() const
{
    std::vector<int> offsets;
    for (const auto &candidate : candidates)
        offsets.push_back(candidate.offset);
    return offsets;
}

} // namespace prefetch
} // namespace gem5
