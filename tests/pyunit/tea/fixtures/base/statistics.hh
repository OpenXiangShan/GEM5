
#pragma once
#include <vector>

namespace gem5 { namespace statistics {
struct Resettable
{
    virtual void reset() = 0; virtual ~Resettable() = default; };
struct Group
{
    std::vector<Resettable*> members;
    Group(Group* = nullptr, const char* = nullptr) {}
    virtual void preDumpStats() {}
    virtual void resetStats() { for (auto *x : members) x->reset(); }
    virtual ~Group() = default;
};
struct Scalar : Resettable
{
    double n = 0;
    Scalar(Group *g = nullptr) { if (g) g->members.push_back(this); }
    Scalar& operator+=(double x) { n += x; return *this; }
    Scalar& operator++() { ++n; return *this; }
    double operator++(int) { return n++; }
    Scalar& operator=(double x) { n = x; return *this; }
    void precision(unsigned) {}
    double value() const { return n; }
    void reset() override { n = 0; }
};
struct Vector : Resettable
{
    std::vector<Scalar> values;
    Vector(Group *g) { g->members.push_back(this); }
    Vector& init(unsigned n) { values.resize(n); return *this; }
    void subname(unsigned, const char*) {}
    void precision(unsigned) {}
    Scalar& operator[](unsigned n) { return values.at(n); }
    void reset() override { for (auto &x : values) x.reset(); }
};
}}
#define ADD_STAT(name, ...) name(this)
