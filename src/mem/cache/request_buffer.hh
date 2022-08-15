#ifndef __MEM_CACHE_REQUEST_BUFFER_HH__
#define __MEM_CACHE_REQUEST_BUFFER_HH__

#include "debug/RequestBuffer.hh"
#include "mem/cache/base.hh"
#include "params/RequestBuffer.hh"
#include "sim/clocked_object.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class RequestBuffer : public ClockedObject
{
  private:
    uint64_t blk_size;
    uint64_t assoc;
    uint64_t cache_size;
    uint64_t num_cache_sets;

    const int capacity;
    /* (packet, blocked) */
    typedef std::pair<PacketPtr, bool> Entry;
    std::list<Entry> queue;

    /* (address, ready_time) */
    std::list<std::pair<Addr, Tick>> hit_req_queue;


  public:
    RequestBuffer(const RequestBufferParams &p);
    ~RequestBuffer();
    void setCacheInfo(uint64_t _cache_sz, uint64_t _assoc, uint64_t _blk_size);
    int numBusyEntries();
    int numFreeEntries();
    int numReadyEntries();
    Addr getCacheSetAddr(Addr pkt_addr) const;
    bool checkConflict(PacketPtr pkt, MSHRQueue &mshrQueue);
    bool isEmpty();
    bool isFull();
    bool enqueue(PacketPtr pkt, MSHRQueue &mshrQueue);
    PacketPtr dequeue();
    void wakeup(Addr blk_addr);
    void wakeup(MSHR *mshr);
    void add_hit_req(Addr blk_addr, Tick ready_time);
    void update_hit_queue();
};




}

#endif
