/*
 * Lightweight statistics shims for BTB predictor unit tests.
 *
 * Production builds use gem5's statistics classes directly.  Unit-test builds
 * only need the tiny counter/vector/distribution API used by predictor logic,
 * so this header provides drop-in stand-ins without pulling in the stats
 * runtime.
 */

#ifndef __CPU_PRED_BTB_TEST_STATS_HH__
#define __CPU_PRED_BTB_TEST_STATS_HH__

#ifdef UNIT_TEST

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{
namespace test_stats
{

class Scalar
{
  private:
    uint64_t value = 0;

  public:
    Scalar() = default;

    Scalar &
    operator++()
    {
        ++value;
        return *this;
    }

    uint64_t
    operator++(int)
    {
        uint64_t old = value;
        ++value;
        return old;
    }

    template <typename T>
    Scalar &
    operator+=(T delta)
    {
        value += static_cast<uint64_t>(delta);
        return *this;
    }

    template <typename T>
    Scalar &
    operator=(T next)
    {
        value = static_cast<uint64_t>(next);
        return *this;
    }

    operator uint64_t() const { return value; }
};

class Vector
{
  private:
    std::vector<uint64_t> values;

  public:
    Vector() = default;

    void init(std::size_t size) { values.assign(size, 0); }

    uint64_t &operator[](std::size_t idx)
    {
        assert(idx < values.size());
        return values[idx];
    }

    uint64_t operator[](std::size_t idx) const
    {
        assert(idx < values.size());
        return values[idx];
    }
};

class Distribution
{
  private:
    int64_t min = 0;
    int64_t max = 0;
    int64_t bucket = 1;
    std::vector<uint64_t> buckets;

  public:
    Distribution() = default;

    void
    init(int64_t min_value, int64_t max_value, int64_t bucket_size)
    {
        assert(bucket_size > 0);
        min = min_value;
        max = max_value;
        bucket = bucket_size;
        buckets.assign((max - min) / bucket + 1, 0);
    }

    void
    sample(int64_t value, uint64_t count = 1)
    {
        if (value < min || value > max) {
            return;
        }
        std::size_t idx = static_cast<std::size_t>((value - min) / bucket);
        assert(idx < buckets.size());
        buckets[idx] += count;
    }
};

} // namespace test_stats
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#else

#include "base/statistics.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{
namespace test_stats = statistics;
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // UNIT_TEST

#endif // __CPU_PRED_BTB_TEST_STATS_HH__
