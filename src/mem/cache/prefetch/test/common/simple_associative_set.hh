#pragma once

#include <cassert>
#include <list>
#include <vector>

#include "base/intmath.hh"
#include "base/types.hh"

namespace gem5
{
namespace prefetch
{
namespace test
{

/**
 * A simple entry class for testing purposes that can be used with
 * SimpleAssociativeSet.
 */
class TestEntry
{
  public:
    TestEntry() : _tag(0), _valid(false) {}
    virtual ~TestEntry() = default;

    Addr getTag() const { return _tag; }
    bool isValid() const { return _valid; }

    void insert(Addr tag)
    {
        _tag = tag;
        _valid = true;
    }

    virtual void invalidate()
    {
        _valid = false;
    }

  protected:
    Addr _tag;
    bool _valid;
};

/**
 * A simplified associative set for unit tests, with a built-in LRU
 * replacement policy. It does not depend on SimObject.
 */
template<class Entry>
class SimpleAssociativeSet
{
  public:
    SimpleAssociativeSet(unsigned num_sets, unsigned assoc);

    Entry* findEntry(Addr addr) const;
    void accessEntry(Entry *entry);
    Entry* findVictim(Addr addr);
    std::vector<Entry *> getPossibleEntries(Addr addr);
    void insertEntry(Addr addr, Entry* entry);
    void invalidate(Entry* entry);

  private:
    void updateLRU(unsigned set, unsigned way);

    unsigned getAddrSet(Addr addr) const;
    Addr getAddrTag(Addr addr) const;

    const unsigned numSets;
    const unsigned associativity;
    const unsigned setMask;
    const unsigned tagShift;

    std::vector<Entry> entries;
    std::vector<std::list<unsigned>> lruLists;
};

template<class Entry>
SimpleAssociativeSet<Entry>::SimpleAssociativeSet(unsigned num_sets,
    unsigned assoc)
  : numSets(num_sets), associativity(assoc),
    setMask(num_sets > 0 ? num_sets - 1 : 0),
    tagShift(floorLog2((uint64_t)num_sets))
{
    assert(isPowerOf2(num_sets));
    assert(isPowerOf2(assoc));

    entries.resize(numSets * associativity);
    lruLists.resize(numSets);

    for (unsigned i = 0; i < numSets; ++i) {
        for (unsigned j = 0; j < associativity; ++j) {
            lruLists[i].push_back(j);
        }
    }
}

template<class Entry>
unsigned
SimpleAssociativeSet<Entry>::getAddrSet(Addr addr) const
{
    return addr & setMask;
}

template<class Entry>
Addr
SimpleAssociativeSet<Entry>::getAddrTag(Addr addr) const
{
    return addr >> tagShift;
}

template<class Entry>
void
SimpleAssociativeSet<Entry>::updateLRU(unsigned set, unsigned way)
{
    lruLists[set].remove(way);
    lruLists[set].push_front(way);
}

template<class Entry>
std::vector<Entry *>
SimpleAssociativeSet<Entry>::getPossibleEntries(Addr addr)
{
    unsigned set = getAddrSet(addr);
    std::vector<Entry*> possible_entries;
    possible_entries.reserve(associativity);
    for (unsigned i = 0; i < associativity; ++i) {
        possible_entries.push_back(&entries[set * associativity + i]);
    }
    return possible_entries;
}

template<class Entry>
Entry*
SimpleAssociativeSet<Entry>::findEntry(Addr addr) const
{
    unsigned set = getAddrSet(addr);
    Addr tag = getAddrTag(addr);
    for (unsigned i = 0; i < associativity; ++i) {
        Entry* entry = const_cast<Entry *>(&entries[set * associativity + i]);
        if (entry->isValid() && entry->getTag() == tag) {
            return entry;
        }
    }
    return nullptr;
}

template<class Entry>
void
SimpleAssociativeSet<Entry>::accessEntry(Entry *entry)
{
    size_t index = entry - &entries[0];
    unsigned set = index / associativity;
    unsigned way = index % associativity;
    updateLRU(set, way);
}

template<class Entry>
Entry*
SimpleAssociativeSet<Entry>::findVictim(Addr addr)
{
    unsigned set = getAddrSet(addr);

    // First, look for an invalid entry
    for (unsigned i = 0; i < associativity; ++i) {
        if (!entries[set * associativity + i].isValid()) {
            return &entries[set * associativity + i];
        }
    }

    // All entries are valid, so find the LRU victim
    unsigned victim_way = lruLists[set].back();
    Entry* victim = &entries[set * associativity + victim_way];
    invalidate(victim);
    return victim;
}

template<class Entry>
void
SimpleAssociativeSet<Entry>::insertEntry(Addr addr, Entry* entry)
{
    entry->insert(getAddrTag(addr));
    accessEntry(entry);
}

template<class Entry>
void
SimpleAssociativeSet<Entry>::invalidate(Entry* entry)
{
    entry->invalidate();
    size_t index = entry - &entries[0];
    unsigned set = index / associativity;
    unsigned way = index % associativity;
    // Move to the back of the LRU list (least-recently used)
    lruLists[set].remove(way);
    lruLists[set].push_back(way);
}

}
}
}
