#ifndef __MEM_CACHE_xs_l3_REQUEST_BUFFER_HH__
#define __MEM_CACHE_xs_l3_REQUEST_BUFFER_HH__

#include <deque>

#include "mem/packet.hh"

namespace gem5
{

class L3RequestBuffer
{
  public:
    explicit L3RequestBuffer(unsigned size);

    bool isFull() const;
    bool empty() const;
    unsigned size() const;
    void push(PacketPtr pkt);
    void pop();
    PacketPtr front();

  private:
    const unsigned _size;
    std::deque<PacketPtr> buffer;
};

} // namespace gem5

#endif // __MEM_CACHE_xs_l3_REQUEST_BUFFER_HH__