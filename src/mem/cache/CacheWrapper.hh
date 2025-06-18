#ifndef __MEM_CACHE_CACHE_WRAPPER_HH__
#define __MEM_CACHE_CACHE_WRAPPER_HH__

#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/CacheWrapper.hh"
#include "sim/clocked_object.hh"
#include "sim/port.hh"

namespace gem5
{

class CacheWrapper : public ClockedObject
{
  private:
    class CPUSidePort : public ResponsePort
    {
      private:
        CacheWrapper *owner;
      public:
        CPUSidePort(const std::string& name, CacheWrapper *owner);
      protected:
        bool recvTimingReq(PacketPtr pkt) override;
        bool recvTimingSnoopResp(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        Tick recvAtomic(PacketPtr pkt) override;
        void recvRespRetry() override;
        AddrRangeList getAddrRanges() const override;
    };

    class MemSidePort : public RequestPort
    {
      private:
        CacheWrapper *owner;
      public:
        MemSidePort(const std::string& name, CacheWrapper *owner);
      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
        void recvTimingSnoopReq(PacketPtr pkt) override;
        void recvRangeChange() override;
        bool isSnooping() const override { return true; }
    };

    class InnerCPUSidePort : public RequestPort
    {
      private:
        CacheWrapper *owner;
      public:
        InnerCPUSidePort(const std::string& name, CacheWrapper *owner);
      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
        void recvTimingSnoopReq(PacketPtr pkt) override;
        void recvRangeChange() override;
        bool isSnooping() const override { return true; }
    };

    class InnerMemSidePort : public ResponsePort
    {
      private:
        CacheWrapper *owner;
      public:
        InnerMemSidePort(const std::string& name, CacheWrapper *owner);
      protected:
        bool recvTimingReq(PacketPtr pkt) override;
        bool recvTimingSnoopResp(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        Tick recvAtomic(PacketPtr pkt) override;
        void recvRespRetry() override;
        AddrRangeList getAddrRanges() const override;
    };

    friend class CPUSidePort;
    friend class MemSidePort;
    friend class InnerCPUSidePort;
    friend class InnerMemSidePort;

    CPUSidePort cpu_side_port;
    MemSidePort mem_side_port;
    InnerCPUSidePort inner_cpu_port;
    InnerMemSidePort inner_mem_port;

  public:
    CacheWrapper(const CacheWrapperParams &p);

    Port &getPort(const std::string &if_name, PortID idx = InvalidPortID) override;
};

} // namespace gem5

#endif // __MEM_CACHE_CACHE_WRAPPER_HH__
