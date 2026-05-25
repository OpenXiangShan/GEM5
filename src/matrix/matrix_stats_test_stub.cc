#include "base/statistics.hh"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "base/logging.hh"
#include "base/stats/info.hh"

namespace gem5
{
namespace statistics
{

std::string Info::separatorString = "::";
int Info::id_count = 0;

Info::Info()
    : flags(none), precision(-1), prereq(0), storageParams()
{
    id = id_count++;
}

Info::~Info()
{
}

const StorageParams *
Info::getStorageParams() const
{
    return storageParams.get();
}

void
Info::setStorageParams(const StorageParams *const params)
{
    storageParams.reset(params);
}

void
Info::setName(const std::string &stat_name, bool old_style)
{
    name = stat_name;
}

bool
Info::less(Info *stat1, Info *stat2)
{
    return stat1->name < stat2->name;
}

bool
Info::baseCheck() const
{
    return true;
}

void
Info::enable()
{
}

void
VectorInfo::enable()
{
}

void
VectorDistInfo::enable()
{
}

void
Vector2dInfo::enable()
{
}

void
InfoAccess::setInfo(Group *parent, Info *info)
{
    _info = info;
}

void
InfoAccess::setParams(const StorageParams *params)
{
    info()->setStorageParams(params);
}

void
InfoAccess::setInit()
{
    info()->flags.set(init);
}

Info *
InfoAccess::info()
{
    panic_if(_info == nullptr, "unit-test stat info is not initialized");
    return _info;
}

const Info *
InfoAccess::info() const
{
    panic_if(_info == nullptr, "unit-test stat info is not initialized");
    return _info;
}

bool
InfoAccess::newStyleStats() const
{
    return _info != nullptr;
}

Group::Group(Group *parent, const char *name)
    : mergedParent(nullptr)
{
    if (parent && name) {
        parent->addStatGroup(name, this);
    } else if (parent && !name) {
        parent->mergeStatGroup(this);
    }
}

Group::~Group()
{
}

void
Group::regStats()
{
}

void
Group::resetStats()
{
    for (auto &stat : stats) {
        stat->reset();
    }
    for (auto &group : mergedStatGroups) {
        group->resetStats();
    }
    for (auto &group : statGroups) {
        group.second->resetStats();
    }
}

void
Group::preDumpStats()
{
}

void
Group::addStat(Info *info)
{
    stats.push_back(info);
    if (mergedParent) {
        mergedParent->addStat(info);
    }
}

void
Group::addStatGroup(const char *name, Group *block)
{
    panic_if(block == nullptr, "Can't add null stat group %s", name);
    panic_if(block == this, "Stat group can't be added to itself");
    panic_if(statGroups.find(name) != statGroups.end(),
        "Stats of the same group share the same name `%s`.\n", name);
    statGroups[name] = block;
}

const Info *
Group::resolveStat(std::string name) const
{
    for (auto &info : stats) {
        if (info->name == name) {
            return info;
        }
    }
    return nullptr;
}

void
Group::mergeStatGroup(Group *block)
{
    panic_if(block == nullptr, "No stat block provided");
    panic_if(block->mergedParent, "Stat group already merged");
    panic_if(block == this, "Stat group can't merge with itself");

    mergedStatGroups.push_back(block);
    for (auto &stat : block->stats) {
        addStat(stat);
    }
    block->mergedParent = this;
}

const std::map<std::string, Group *> &
Group::getStatGroups() const
{
    return statGroups;
}

const std::vector<Info *> &
Group::getStats() const
{
    return stats;
}

} // namespace statistics
} // namespace gem5
