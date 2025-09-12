#pragma once

#include <memory>
#include <vector>

#include "base/types.hh"

namespace gem5
{
namespace prefetch
{
namespace test
{

// Helper function for printf
// #define printf_wrapper(...) printf(__VA_ARGS__)
// use following to disable printf
#define printf_wrapper(...)

// Forward declaration
class BOP;

// Mock for PrefetchSourceType
enum class PrefetchSourceType
{
    HWP_BOP
};

// Mock for AddrPriority
struct AddrPriority
{
    Addr addr;
    int priority;
    PrefetchSourceType src;

    AddrPriority(Addr a, int p, PrefetchSourceType s) : addr(a), priority(p), src(s) {}
    Addr getAddr() const { return addr; }
};

// Mock for Packet/PacketPtr
struct Packet
{
    };
using PacketPtr = std::shared_ptr<Packet>;

// Mock for PrefetchInfo
class PrefetchInfo
{
private:
    Addr address = 0;
    bool is_secure = false;

public:
    PrefetchInfo() = default;
    PrefetchInfo(Addr addr, bool secure) : address(addr), is_secure(secure) {}
    Addr getAddr() const { return address; }
    bool isSecure() const { return is_secure; }
};

} // namespace test
} // namespace prefetch
} // namespace gem5
