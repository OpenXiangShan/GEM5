
#include "request_buffer.hh"

namespace gem5
{

RequestBuffer::RequestBuffer(const RequestBufferParams &p)
    : ClockedObject(p), capacity(0)
{
    queue = std::list<Entry>();
    hit_req_queue = std::list<std::pair<Addr, Tick>>();
}

RequestBuffer::~RequestBuffer() {}


int
RequestBuffer::numBusyEntries()
{
    return queue.size();
}
int
RequestBuffer::numFreeEntries()
{
    if (capacity > 0) {
        int free = capacity - numBusyEntries();
        assert(free >= 0);
        return free;
    }
    return -1;
}
int
RequestBuffer::numReadyEntries()
{
    int ret = 0;
    for (auto &entry : queue) {
        if (!entry.second)
            ret++;
    }
    return ret;
}
void
RequestBuffer::setCacheInfo(uint64_t _cache_sz, uint64_t _assoc,
                            uint64_t _blk_size)
{
    cache_size = _cache_sz;
    assoc = _assoc;
    blk_size = _blk_size;
    num_cache_sets = cache_size / assoc / blk_size;
    DPRINTF(
        RequestBuffer,
        "RequestBuffer cache_size: %d, assoc: %d, blk_size: %d, sets: %d\n",
        cache_size, assoc, blk_size, num_cache_sets);
}
Addr
RequestBuffer::getCacheSetAddr(Addr pkt_addr) const
{
    return (pkt_addr / blk_size) & (num_cache_sets - 1);
}
bool
RequestBuffer::checkConflict(PacketPtr pkt, MSHRQueue &mshrQueue)
{
    return mshrQueue.findIf<Addr>(
        [this](MSHR *mshr) { return getCacheSetAddr(mshr->blkAddr); },
        getCacheSetAddr(pkt->getAddr()));
}
bool
RequestBuffer::isEmpty()
{
    return queue.empty();
}
bool
RequestBuffer::isFull()
{
    return numFreeEntries() == 0;
}
bool
RequestBuffer::enqueue(PacketPtr pkt, MSHRQueue &mshrQueue)
{
    if (isFull()) {
        DPRINTF(RequestBuffer,
                "Attempting to enqueue request buffer but it is "
                "full!\npacket: [%s]\n",
                pkt->print());
        return false;
    } else {
        bool conflict = checkConflict(pkt, mshrQueue);
        DPRINTF(
            RequestBuffer,
            "Enqueue request buffer, set: [%d] conflict: [%d]\npacket: [%s]\n",
            getCacheSetAddr(pkt->getAddr()), conflict, pkt->print());
        queue.emplace_back(pkt, conflict);
        return true;
    }
}
PacketPtr
RequestBuffer::dequeue()
{
    std::list<Entry>::iterator it;
    for (it = queue.begin(); it != queue.end(); it++) {
        if (!it->second) {
            PacketPtr pkt = it->first;
            DPRINTF(RequestBuffer,
                    "Dequeue request buffer, set: [%x]\npacket: [%s]\n",
                    getCacheSetAddr(pkt->getAddr()), pkt->print());
            wakeup(pkt->getAddr());
            queue.erase(it--);
            return pkt;
        }
    }
    return nullptr;
}
void
RequestBuffer::wakeup(MSHR *mshr)
{
    wakeup(mshr->blkAddr);
}
void
RequestBuffer::wakeup(Addr blk_addr)
{
    Addr key = getCacheSetAddr(blk_addr);
    for (auto &entry : queue) {
        if (getCacheSetAddr(entry.first->getAddr()) == key) {
            DPRINTF(RequestBuffer,
                    "Packet wakeup-ed by addr: [%lx]\ncurrent packet: %s\n",
                    blk_addr, entry.first->print());
            entry.second = false;
        }
    }
}
void
RequestBuffer::add_hit_req(Addr blk_addr, Tick ready_time)
{
    DPRINTF(RequestBuffer, "Adding %lx to hit queue, ready time: %ld\n",
            blk_addr, ready_time);
    hit_req_queue.emplace_back(blk_addr, ready_time);
}
void
RequestBuffer::update_hit_queue()
{
    DPRINTF(RequestBuffer, "Updating hit queue\n");
    Tick cur = curTick();
    for (auto it = hit_req_queue.begin(); it != hit_req_queue.end(); it++) {
        if (it->second <= cur) {
            DPRINTF(RequestBuffer, "Hit Queue: using %lx to wakeup others\n",
                    it->first);
            wakeup(it->first);
            hit_req_queue.erase(it--);
        }
    }
}




}
