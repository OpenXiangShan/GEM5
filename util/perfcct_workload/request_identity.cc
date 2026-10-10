// Host-side identity checks; no simulation timing or modeled state is changed.
#include <cassert>
#include <memory>
#include <new>
#include <type_traits>

#include "mem/request.hh"

int
main()
{
    auto original = std::make_shared<gem5::Request>();
    auto same = original;
    const auto first = original->causalTraceID();
    assert(first != 0 && same->causalTraceID() == first);

    gem5::Request copy(*original);
    const auto copied = copy.causalTraceID();
    assert(copied != first && copy.causalTraceID() == copied);

    // Reusing storage must not resurrect the old object's identity.
    std::aligned_storage_t<sizeof(gem5::Request), alignof(gem5::Request)> storage;
    auto *old = new (&storage) gem5::Request();
    const auto previous = old->causalTraceID();
    old->~Request();
    auto *replacement = new (&storage) gem5::Request(copy);
    const auto current = replacement->causalTraceID();
    assert(current != previous && current != copied && current != first);
    replacement->~Request();
}
