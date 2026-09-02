#include "cpu/o3/crob_kmhv2_break_analysis.hh"

#include <stdexcept>

namespace gem5::o3
{

namespace
{

uint64_t
ceilDiv(uint64_t value, unsigned divisor)
{
    return value == 0 ? 0 : 1 + (value - 1) / divisor;
}

void
analyzeFtqDomain(const std::vector<Kmhv2InstSample> &samples,
                 size_t begin, size_t end, unsigned group_width,
                 Kmhv2BundleAnalysis &result)
{
    uint64_t simple_count = 0;
    uint64_t complex_count = 0;
    bool has_simple_before = false;
    size_t index = begin;

    while (index < end) {
        if (isKmhv2SimpleClass(samples[index].instClass)) {
            const size_t run_begin = index;
            while (index < end &&
                   isKmhv2SimpleClass(samples[index].instClass)) {
                ++index;
            }

            const size_t run_length = index - run_begin;
            result.simpleRunLengths[run_length]++;
            simple_count += run_length;
            result.physicalEntries += ceilDiv(run_length, group_width);
            has_simple_before = true;
            continue;
        }

        const size_t complex_begin = index;
        while (index < end &&
               !isKmhv2SimpleClass(samples[index].instClass)) {
            ++index;
        }

        const size_t complex_length = index - complex_begin;
        complex_count += complex_length;
        result.physicalEntries += complex_length;

        const bool has_simple_after =
            index < end && isKmhv2SimpleClass(samples[index].instClass);
        if (has_simple_before && has_simple_after) {
            result.breakBlocks++;
            for (size_t i = complex_begin; i < index; ++i) {
                result.breakingClassCounts[
                    static_cast<size_t>(samples[i].instClass)]++;
            }
        }
    }

    result.noBreakPhysicalEntries +=
        complex_count + ceilDiv(simple_count, group_width);
}

} // anonymous namespace

bool
isKmhv2SimpleClass(Kmhv2InstClass inst_class)
{
    return inst_class == Kmhv2InstClass::SimpleIntegerAlu ||
           inst_class == Kmhv2InstClass::SimpleFloatingAlu ||
           inst_class == Kmhv2InstClass::SimpleOther;
}

Kmhv2BundleAnalysis
analyzeKmhv2Bundle(const std::vector<Kmhv2InstSample> &samples,
                   unsigned group_width)
{
    if (group_width == 0) {
        throw std::invalid_argument("kmhv2 group width must be non-zero");
    }

    Kmhv2BundleAnalysis result;
    result.simpleRunLengths.resize(samples.size() + 1, 0);

    for (const auto &sample : samples) {
        result.classCounts[static_cast<size_t>(sample.instClass)]++;
    }

    size_t domain_begin = 0;
    while (domain_begin < samples.size()) {
        size_t domain_end = domain_begin + 1;
        while (domain_end < samples.size() &&
               samples[domain_end].ftqId == samples[domain_begin].ftqId) {
            ++domain_end;
        }
        analyzeFtqDomain(samples, domain_begin, domain_end, group_width,
                         result);
        domain_begin = domain_end;
    }

    result.breakLostEntries =
        result.physicalEntries - result.noBreakPhysicalEntries;
    return result;
}

} // namespace gem5::o3
