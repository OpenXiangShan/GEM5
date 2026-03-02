#include "mem/cache/xs_l3/L3RequestBuffer.hh"

#include "base/trace.hh"

namespace gem5
{
L3RequestBuffer::L3RequestBuffer(unsigned size)
    : _size(size)
{
}


bool
L3RequestBuffer::isFull() const
{
    return buffer.size() >= _size;
}

bool
L3RequestBuffer::empty() const
{
    return buffer.empty();
}

unsigned
L3RequestBuffer::size() const
{
    return buffer.size();
}

void
L3RequestBuffer::push(PacketPtr pkt)
{
    fatal_if(isFull(), "L3RequestBuffer is full");
    buffer.push_back(pkt);
}

void
L3RequestBuffer::pop()
{
    fatal_if(empty(), "L3RequestBuffer is empty");
    buffer.pop_front();
}

PacketPtr
L3RequestBuffer::front()
{
    fatal_if(empty(), "L3RequestBuffer is empty");
    return buffer.front();
}

} // namespace gem5
