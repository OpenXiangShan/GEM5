// L0/L1 hybrid tests for the trace address-mapping decision functions.
//
// Anchored: the tests call the production statics TraceReader::mapAddress* /
// mapTrace*ToVirtual directly (the same symbols the readers use), so a
// semantic change in the mapping logic fails here. No mirrored logic.
//
// Tombstones (per mapping domain, from the trace-mode fix history):
//   - 6adcc80da4  "Use TheISA::PageBytes": linear page-aligned mapping must
//     stay continuous across page boundaries (host PAGE_SIZE broke it).
//   - b811be1b87  "align trace mapping window to physmem": the mapped window
//     is clamped to [base, base + size); wrap is in-window, never past the
//     physical memory region.
//   - a7da33710e  "keep trace mapped addresses unaligned": the PC path
//     preserves 2-byte (compressed) spacing; it must not be 4-byte aligned.
//     (The memory path deliberately masks the low 2 bits — anchored below
//     as current contract, so any change is a conscious one.)
//   - 06a611df32  (review hardening): a page-aligned window smaller than a
//     page fails closed (panic) instead of mapping out of window.
//   - 7a01b49499  "Default trace address mapping to linear": mode dispatch
//     "linear" -> linear mapping, anything else -> hash mapping.
//
// Build (NULL ISA):
//   scons build/NULL/cpu/o3/trace_addr_map.test.opt --unit-test -j$(nproc)
// Run:
//   ./build/NULL/cpu/o3/trace_addr_map.test.opt

#include <cstdint>
#include <string>

#include "config/the_isa.hh"
#include "cpu/o3/trace/TraceReader.hh"
#include "gtest/gtest.h"

namespace
{

using gem5::o3::TraceReader;
using AddrMapConfig = TraceReader::AddrMapConfig;

// TheISA::PageBytes is 4096 for both NULL (arch/null/page_size.hh) and
// RISCV, so the numeric expectations below hold in every build variant.
constexpr uint64_t kPageBytes = 4096;

AddrMapConfig
makeConfig(uint64_t base, uint64_t size, const std::string &mode,
           bool pageAlign)
{
    return AddrMapConfig{base, size, mode, pageAlign};
}

} // anonymous namespace

TEST(TraceAddrMap, LinearSequentialLocalityWithoutPageAlign)
{
    // Consecutive trace addresses map to consecutive virtual addresses
    // (fetch locality); no alignment is imposed by the base mapping.
    const auto cfg = makeConfig(0x10000000, 0x100000, "linear", false);
    EXPECT_EQ(TraceReader::mapAddressLinear(0x1000, cfg), 0x10001000u);
    EXPECT_EQ(TraceReader::mapAddressLinear(0x1001, cfg), 0x10001001u);
    EXPECT_EQ(TraceReader::mapAddressLinear(0x1002, cfg), 0x10001002u);
    EXPECT_EQ(TraceReader::mapAddressLinear(0x1004, cfg), 0x10001004u);
}

TEST(TraceAddrMap, LinearPageAlignedStaysContinuousAcrossPageBoundary)
{
    // 6adcc80da4: mapping must use TheISA::PageBytes so consecutive
    // addresses remain consecutive across a page boundary.
    const auto cfg = makeConfig(0x20000000, 0x100000, "linear", true);
    EXPECT_EQ(TraceReader::mapAddressLinear(0x0FFC, cfg), 0x20000FFCu);
    EXPECT_EQ(TraceReader::mapAddressLinear(0x0FFD, cfg), 0x20000FFDu);
    EXPECT_EQ(TraceReader::mapAddressLinear(0x0FFE, cfg), 0x20000FFEu);
    EXPECT_EQ(TraceReader::mapAddressLinear(0x0FFF, cfg), 0x20000FFFu);
    EXPECT_EQ(TraceReader::mapAddressLinear(0x1000, cfg), 0x20001000u);
    EXPECT_EQ(TraceReader::mapAddressLinear(0x1001, cfg), 0x20001001u);
}

TEST(TraceAddrMap, LinearPageAlignedWrapStaysInsideWindow)
{
    // b811be1b87: the window is [base, base + size); once the trace page
    // count exceeds pages_in_region the mapping wraps in-window instead of
    // running past the physical memory region.
    const auto cfg = makeConfig(0x20000000, 0x100000, "linear", true);
    const uint64_t pages_in_region = 0x100000 / kPageBytes;  // 64
    // First page of the second pass through the region maps back to base.
    EXPECT_EQ(TraceReader::mapAddressLinear(pages_in_region * kPageBytes, cfg),
              0x20000000u);
    EXPECT_EQ(
        TraceReader::mapAddressLinear((pages_in_region + 1) * kPageBytes, cfg),
        0x20000000u + kPageBytes);
    // Offsets survive the wrap.
    EXPECT_EQ(
        TraceReader::mapAddressLinear(pages_in_region * kPageBytes + 0x123, cfg),
        0x20000123u);
}

TEST(TraceAddrMap, LinearMappingNeverExceedsWindow)
{
    const auto cfg = makeConfig(0x20000000, 0x100000, "linear", true);
    for (uint64_t trace_page = 0; trace_page < 4096; ++trace_page) {
        const uint64_t offsets[] = {0u, 1u, 2u, kPageBytes - 2u,
                                    kPageBytes - 1u};
        for (uint64_t off : offsets) {
            const uint64_t mapped = TraceReader::mapAddressLinear(
                trace_page * kPageBytes + off, cfg);
            EXPECT_GE(mapped, 0x20000000u);
            EXPECT_LT(mapped, 0x20000000u + 0x100000);
        }
    }
}

TEST(TraceAddrMap, PcMappingPreservesTwoByteSpacing)
{
    // a7da33710e: the PC path must preserve compressed (2-byte) instruction
    // spacing — no 4-byte alignment forcing on instruction addresses.
    const auto cfg = makeConfig(0x30000000, 0x100000, "linear", true);
    const uint64_t pc_a = TraceReader::mapTracePcToVirtual(0x2003, cfg);
    const uint64_t pc_b = TraceReader::mapTracePcToVirtual(0x2005, cfg);
    EXPECT_EQ(pc_b - pc_a, 2u) << "2-byte instruction spacing must survive";
    EXPECT_EQ(pc_a & 0x1, 1u) << "odd PC must stay odd (unaligned)";
}

TEST(TraceAddrMap, MemMappingAlignsToFourBytes)
{
    // Current contract (anchored, not endorsed): the memory path masks the
    // low two bits. If this is ever changed intentionally, update this test
    // in the same commit.
    const auto cfg = makeConfig(0x30000000, 0x100000, "linear", true);
    const uint64_t mem = TraceReader::mapTraceMemToVirtual(0x2002, cfg);
    EXPECT_EQ(mem & 0x3, 0u);
}

TEST(TraceAddrMap, HashMappingStaysInsideWindow)
{
    const auto cfg = makeConfig(0x40000000, 0x100000, "hash", false);
    for (uint64_t addr = 0; addr < (1u << 26); addr += 4099) {
        const uint64_t mapped = TraceReader::mapAddressHash(addr, cfg);
        EXPECT_GE(mapped, 0x40000000u);
        EXPECT_LT(mapped, 0x40000000u + 0x100000);
    }
}

TEST(TraceAddrMap, HashPageAlignedPreservesPageOffset)
{
    // pageAlign preserves the intra-page offset, so compressed spacing
    // within a page survives hash mapping too.
    const auto cfg = makeConfig(0x40000000, 0x100000, "hash", true);
    for (uint64_t addr = 0; addr < (1u << 22); addr += 2053) {
        const uint64_t mapped = TraceReader::mapAddressHash(addr, cfg);
        EXPECT_EQ(mapped % kPageBytes, addr % kPageBytes);
    }
}

TEST(TraceAddrMap, ModeDispatchLinearVsHash)
{
    // 7a01b49499: "linear" selects the linear mapping; any other mode
    // string falls back to hash. Both dispatch arms are anchored.
    const auto lin = makeConfig(0x10000000, 0x100000, "linear", false);
    const auto hsh = makeConfig(0x10000000, 0x100000, "hash", false);
    const auto other = makeConfig(0x10000000, 0x100000, "weird-mode", false);

    EXPECT_EQ(TraceReader::mapTraceAddressToVirtual(0x1234, lin),
              TraceReader::mapAddressLinear(0x1234, lin));
    EXPECT_EQ(TraceReader::mapTraceAddressToVirtual(0x1234, hsh),
              TraceReader::mapAddressHash(0x1234, hsh));
    EXPECT_EQ(TraceReader::mapTraceAddressToVirtual(0x1234, other),
              TraceReader::mapAddressHash(0x1234, other));
}

TEST(TraceAddrMap, ZeroSizeMappingPanics)
{
    // Fail-closed contract: a zero-sized mapping window must not silently
    // produce addresses; it panics. Unit-test builds convert panic into an
    // exception (see base/gtest/logging_mock.cc), hence EXPECT_ANY_THROW.
    const auto cfg = makeConfig(0x10000000, 0, "linear", false);
    EXPECT_ANY_THROW(TraceReader::mapTraceAddressToVirtual(0x1000, cfg));
}

TEST(TraceAddrMap, PageAlignedWindowSmallerThanPagePanics)
{
    // 06a611df32 review hardening: pageAlign with size < PageBytes cannot
    // hold a single page; fail closed instead of mapping out of window.
    const auto cfg = makeConfig(0x10000000, kPageBytes / 2, "linear", true);
    EXPECT_ANY_THROW(TraceReader::mapAddressLinear(0x1000, cfg));
}
