// Policy checks for idle-dispatch stall accounting.
//
// Build and run (no gem5 binary required):
//   g++ -std=c++17 -Wall -Wextra -Werror -Isrc -o /tmp/fetch_supply_reason.test src/cpu/o3/fetch_supply_reason.test.cc
//   /tmp/fetch_supply_reason.test
//
// Covers the two review notes on #1066:
// 1. A drained frontend must not be published as full-width NoStall.
// 2. An outstanding ITLB walk must stay ITlb, including cycles after the
//    request was issued, when the invalid buffer is still latched as Icache.

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "cpu/o3/fetch_supply_reason.hh"

using namespace gem5::o3;

namespace
{

int failures = 0;

void
expect(bool ok, const std::string &what)
{
    if (ok)
        return;
    std::cerr << "FAIL " << what << "\n";
    ++failures;
}

void
expectSlot(SupplySlot got, SupplySlot want, const std::string &what)
{
    expect(got == want, what);
}

} // namespace

int
main()
{
    // Issue cycle and every later TlbWait cycle: buffer is invalid, so the
    // cut is Icache, but the walk has not reached the cache.
    expectSlot(zeroFetchSupplySlot(true, FetchSupplyCut::Icache, false),
               SupplySlot::ITlb, "tlb wait outranks icache cut");
    expectSlot(zeroFetchSupplySlot(true, FetchSupplyCut::Icache, true),
               SupplySlot::ITlb, "later tlb-wait cycle still ITlb");
    expectSlot(zeroFetchSupplySlot(true, FetchSupplyCut::None, true),
               SupplySlot::ITlb, "tlb wait with no cut");
    expectSlot(zeroFetchSupplySlot(true, FetchSupplyCut::Stream, true),
               SupplySlot::ITlb, "zero supply keeps tlb over a stream latch");

    // Real I$ miss: translation is done, the cache has not returned the line.
    expectSlot(zeroFetchSupplySlot(false, FetchSupplyCut::Icache, false),
               SupplySlot::Icache, "cache wait uses icache cut");
    expectSlot(zeroFetchSupplySlot(false, FetchSupplyCut::Stream, true),
               SupplySlot::Stream, "taken or FTQ end");
    expectSlot(zeroFetchSupplySlot(false, FetchSupplyCut::Buf, true),
               SupplySlot::Buf, "walked off the fetch buffer");

    // No new cut this cycle. Keep a reason fetch already recorded (FTQ bubble,
    // trap, ...). If nothing was recorded, the empty transfer is OtherFetch.
    expectSlot(zeroFetchSupplySlot(false, FetchSupplyCut::None, false),
               SupplySlot::KeepAlready, "keep an already-set reason");
    expectSlot(zeroFetchSupplySlot(false, FetchSupplyCut::None, true),
               SupplySlot::OtherFetch, "empty supply with no reason");

    expectSlot(partialFetchSupplySlot(true, FetchSupplyCut::Icache),
               SupplySlot::ITlb, "partial refill during tlb walk");
    expectSlot(partialFetchSupplySlot(true, FetchSupplyCut::Stream),
               SupplySlot::Stream, "partial taken cut stays stream");
    expectSlot(partialFetchSupplySlot(false, FetchSupplyCut::Icache),
               SupplySlot::Icache, "partial real icache miss");
    expectSlot(partialFetchSupplySlot(false, FetchSupplyCut::None),
               SupplySlot::FetchFrag, "partial with no cut stays frag");

    // Drained buffers, backend not blocking. Each stage must forward the
    // upstream reason. Leaving the tick-start NoStall in place is the bug.
    constexpr int noStall = 0;
    constexpr int icache = 1;
    constexpr int itlb = 2;
    std::vector<int> decode(4, noStall);
    const std::vector<int> fetch(4, icache);
    copyUpstreamStall(decode, fetch, noStall);
    expect(decode == fetch, "decode publishes fetch Icache, not NoStall");

    std::vector<int> rename(4, noStall);
    copyUpstreamStall(rename, decode, noStall);
    expect(rename == fetch, "rename publishes decode's Icache");

    std::vector<int> dispatch(4, noStall);
    copyUpstreamStall(dispatch, rename, noStall);
    expect(dispatch == std::vector<int>(4, icache),
           "dispatch counts Icache on a drained miss");

    std::vector<int> tlbDecode(4, noStall);
    copyUpstreamStall(tlbDecode, std::vector<int>(4, itlb), noStall);
    expect(tlbDecode == std::vector<int>(4, itlb),
           "drained tlb wait is not NoStall either");

    std::vector<int> wide(4, noStall);
    copyUpstreamStall(wide, std::vector<int>{icache, icache}, noStall);
    expect(wide == std::vector<int>({icache, icache, noStall, noStall}),
           "a short upstream vector does not read off the end");

    std::vector<int> untouched(4, noStall);
    copyUpstreamStall(untouched, std::vector<int>{}, noStall);
    expect(untouched == std::vector<int>(4, noStall),
           "empty upstream (startup) stays NoStall");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "fetch supply reason checks passed\n";
    return EXIT_SUCCESS;
}
