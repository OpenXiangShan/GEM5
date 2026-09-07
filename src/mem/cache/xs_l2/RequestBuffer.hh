#ifndef __MEM_CACHE_XS_L2_REQUEST_BUFFER_HH__
#define __MEM_CACHE_XS_L2_REQUEST_BUFFER_HH__

#include <deque>

#include "mem/cache/xs_l2/TaskSource.hh"
#include "mem/packet.hh"

namespace gem5
{

class RequestBuffer
{
  public:
    struct Entry
    {
        PacketPtr pkt;
        TaskSource source;
    };

    explicit RequestBuffer(unsigned size);

    bool isFull() const;
    bool empty() const;
    unsigned size() const;
    void push(PacketPtr pkt, TaskSource source);
    void pop();
    Entry front() const;

  private:
    const unsigned _size;
    std::deque<Entry> buffer;
};

} // namespace gem5

#endif // __MEM_CACHE_XS_L2_REQUEST_BUFFER_HH__
