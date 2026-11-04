
#pragma once
namespace gem5 { namespace o3 {
class DynInst;
class DynInstPtr
{
    DynInst *value = nullptr;
public:
    DynInstPtr() = default;
    DynInstPtr(DynInst *p) : value(p) {}
    DynInst* operator->() const { return value; }
    explicit operator bool() const { return value; }
};
}}
