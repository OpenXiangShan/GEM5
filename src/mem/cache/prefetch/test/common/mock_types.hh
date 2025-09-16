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
    HWP_BOP,
    CMC,
    SStream,
    SPht
};

// Mock for AddrPriority
struct AddrPriority
{
    Addr addr;
    int priority;
    PrefetchSourceType src;
    bool pfahead;
    int pfahead_host;

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
    Addr pc = 0;
    bool is_secure = false;
    bool write = false;
    bool cacheMiss = false;
public:
    PrefetchInfo() = default;
    PrefetchInfo(Addr addr, bool secure) : address(addr), is_secure(secure) {}
    Addr getAddr() const { return address; }
    void setAddr(Addr addr) { address = addr; }
    Addr getPC() const { return pc; }
    void setPC(Addr pc) { this->pc = pc; }
    bool hasPC() const { return pc != 0; }
    bool isSecure() const { return is_secure; }
    void setSecure(bool secure) { is_secure = secure; }
    bool isWrite() const { return write; }
    void setWrite(bool write) { this->write = write; }
    bool isCacheMiss() const { return cacheMiss; }
    void setCacheMiss(bool cacheMiss) { this->cacheMiss = cacheMiss; }
};

} // namespace test
} // namespace prefetch
} // namespace gem5
