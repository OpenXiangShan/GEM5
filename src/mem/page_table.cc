/*
 * EmulationPageTable implementation (SE-mode page table)
 */

#include "mem/page_table.hh"

#include <algorithm>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/MMU.hh"
#include "sim/faults.hh"

namespace gem5
{

void
EmulationPageTable::map(Addr vaddr, Addr paddr, int64_t size, uint64_t flags)
{
    // starting address must be page aligned
    assert(pageOffset(vaddr) == 0);
    assert(pageOffset(paddr) == 0);
    assert(size > 0);

    for (int64_t offset = 0; offset < size; offset += _pageSize) {
        Addr vpage = vaddr + offset;
        Addr ppage = paddr + offset;
        if (pTable.find(vpage) != pTable.end()) {
            if (!(flags & MappingFlags::Clobber))
                panic("EmulationPageTable: remapping virtual page %#x without clobber", vpage);
        }
        pTable[vpage] = Entry(ppage, flags);
    }
}

void
EmulationPageTable::remap(Addr vaddr, int64_t size, Addr new_vaddr)
{
    assert(pageOffset(vaddr) == 0);
    assert(pageOffset(new_vaddr) == 0);
    for (int64_t offset = 0; offset < size; offset += _pageSize) {
        auto new_it = pTable.find(new_vaddr + offset);
        auto old_it = pTable.find(vaddr + offset);
        assert(old_it != pTable.end() && new_it == pTable.end());
        auto e = old_it->second;
        pTable.erase(old_it);
        pTable[new_vaddr + offset] = e;
    }
}

void
EmulationPageTable::unmap(Addr vaddr, int64_t size)
{
    assert(pageOffset(vaddr) == 0);
    for (int64_t offset = 0; offset < size; offset += _pageSize) {
        auto it = pTable.find(vaddr + offset);
        assert(it != pTable.end());
        pTable.erase(it);
    }
}

bool
EmulationPageTable::isUnmapped(Addr vaddr, int64_t size)
{
    assert(pageOffset(vaddr) == 0);
    for (int64_t offset = 0; offset < size; offset += _pageSize) {
        if (pTable.find(vaddr + offset) != pTable.end())
            return false;
    }
    return true;
}

const EmulationPageTable::Entry *
EmulationPageTable::lookup(Addr vaddr)
{
    Addr page_addr = pageAlign(vaddr);
    auto iter = pTable.find(page_addr);
    if (iter == pTable.end())
        return nullptr;
    return &(iter->second);
}

bool
EmulationPageTable::translate(Addr vaddr, Addr &paddr)
{
    const Entry *entry = lookup(vaddr);
    if (!entry) {
        DPRINTF(MMU, "Couldn't Translate: %#x\n", vaddr);
        return false;
    }
    paddr = pageOffset(vaddr) + entry->paddr;
    DPRINTF(MMU, "Translating: %#x->%#x\n", vaddr, paddr);
    return true;
}

Fault
EmulationPageTable::translate(const RequestPtr &req)
{
    Addr paddr;
    assert(pageAlign(req->getVaddr() + req->getSize() - 1) ==
           pageAlign(req->getVaddr()));
    if (!translate(req->getVaddr(), paddr))
    {
        int ctx = req->hasContextId() ? req->contextId() : -1;
        DPRINTF(MMU, "[PT] translate fault: vaddr=%#x size=%u ctx=%d\n",
                req->getVaddr(), req->getSize(), ctx);
        return Fault(new GenericPageTableFault(req->getVaddr()));
    }
    req->setPaddr(paddr);
    if ((paddr & (_pageSize - 1)) + req->getSize() > _pageSize) {
        panic("Request spans page boundaries!\n");
        return NoFault;
    }
    return NoFault;
}

void
EmulationPageTable::PageTableTranslationGen::translate(Range &range) const
{
    const Addr page_size = pt->pageSize();

    Addr next = roundUp(range.vaddr, page_size);
    if (next == range.vaddr)
        next += page_size;
    range.size = std::min(range.size, next - range.vaddr);

    if (!pt->translate(range.vaddr, range.paddr))
        range.fault = Fault(new GenericPageTableFault(range.vaddr));
}

const std::string
EmulationPageTable::externalize() const
{
    std::string out;
    for (const auto &kv : pTable) {
        out += csprintf("%#x:%#x;", kv.first, kv.second.paddr);
    }
    return out;
}

void
EmulationPageTable::getMappings(std::vector<std::pair<Addr, Addr>> *addr_mappings)
{
    addr_mappings->clear();
    addr_mappings->reserve(pTable.size());
    for (const auto &kv : pTable) {
        addr_mappings->emplace_back(kv.first, kv.second.paddr);
    }
}

void
EmulationPageTable::serialize(CheckpointOut &cp) const
{
    ScopedCheckpointSection sec(cp, "ptable");
    paramOut(cp, "size", pTable.size());
    size_t count = 0;
    for (auto &pte : pTable) {
        ScopedCheckpointSection ent(cp, csprintf("Entry%d", count++));
        paramOut(cp, "vaddr", pte.first);
        paramOut(cp, "paddr", pte.second.paddr);
        paramOut(cp, "flags", pte.second.flags);
    }
}

void
EmulationPageTable::unserialize(CheckpointIn &cp)
{
    int count;
    ScopedCheckpointSection sec(cp, "ptable");
    paramIn(cp, "size", count);
    for (int i = 0; i < count; ++i) {
        ScopedCheckpointSection ent(cp, csprintf("Entry%d", i));
        Addr vaddr; Addr paddr; uint64_t flags;
        paramIn(cp, "vaddr", vaddr);
        paramIn(cp, "paddr", paddr);
        paramIn(cp, "flags", flags);
        pTable.emplace(vaddr, Entry(paddr, flags));
    }
}

} // namespace gem5
