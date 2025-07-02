#include "mem/cache/xs_l2/RequestBuffer.hh"

#include "base/trace.hh"

namespace gem5
{
RequestBuffer::RequestBuffer(unsigned size)
    : _size(size)
{
}


bool
RequestBuffer::isFull() const
{
    return buffer.size() >= _size;
}

bool
RequestBuffer::empty() const
{
    return buffer.empty();
}

unsigned
RequestBuffer::size() const
{
    return buffer.size();
}

void
RequestBuffer::push(PacketPtr pkt)
{
    fatal_if(isFull(), "RequestBuffer is full");
    buffer.push_back(pkt);
}

void
RequestBuffer::pop()
{
    fatal_if(empty(), "RequestBuffer is empty");
    buffer.pop_front();
}

PacketPtr
RequestBuffer::front()
{
    fatal_if(empty(), "RequestBuffer is empty");
    return buffer.front();
}

} // namespace gem5
