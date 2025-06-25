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
        bool recvTimingReq(PacketPtr pkt) override {
          return owner->cpuSidePortRecvTimingReq(pkt);
        }
        bool recvTimingSnoopResp(PacketPtr pkt) override {
          return owner->cpuSidePortRecvTimingSnoopResp(pkt);
        }
        void recvFunctional(PacketPtr pkt) override {
          return owner->cpuSidePortRecvFunctional(pkt);
        }
        Tick recvAtomic(PacketPtr pkt) override {
          return owner->cpuSidePortRecvAtomic(pkt);
        }
        void recvRespRetry() override {
          return owner->cpuSidePortRecvRespRetry();
        }
        AddrRangeList getAddrRanges() const override {
          return owner->cpuSidePortGetAddrRanges();
        }
    };

    class MemSidePort : public RequestPort
    {
      private:
        CacheWrapper *owner;
      public:
        MemSidePort(const std::string& name, CacheWrapper *owner);
      protected:
        bool recvTimingResp(PacketPtr pkt) override {
          return owner->memSidePortRecvTimingResp(pkt);
        }
        void recvReqRetry() override {
          return owner->memSidePortRecvReqRetry();
        }
        void recvTimingSnoopReq(PacketPtr pkt) override {
          return owner->memSidePortRecvTimingSnoopReq(pkt);
        }
        void recvRangeChange() override {
          return owner->memSidePortRecvRangeChange();
        }
        bool isSnooping() const override {
          return true;
        }
    };

    class InnerCPUSidePort : public RequestPort
    {
      private:
        CacheWrapper *owner;
      public:
        InnerCPUSidePort(const std::string& name, CacheWrapper *owner);
      protected:
        bool recvTimingResp(PacketPtr pkt) override {
          return owner->innerCpuPortRecvTimingResp(pkt);
        }
        void recvReqRetry() override {
          return owner->innerCpuPortRecvReqRetry();
        }
        void recvTimingSnoopReq(PacketPtr pkt) override {
          return owner->innerCpuPortRecvTimingSnoopReq(pkt);
        }
        void recvRangeChange() override {
          return owner->innerCpuPortRecvRangeChange();
        }
        bool isSnooping() const override {
          return true;
        }
    };

    class InnerMemSidePort : public ResponsePort
    {
      private:
        CacheWrapper *owner;
      public:
        InnerMemSidePort(const std::string& name, CacheWrapper *owner);
      protected:
        bool recvTimingReq(PacketPtr pkt) override {
          return owner->innerMemPortRecvTimingReq(pkt);
        }
        bool recvTimingSnoopResp(PacketPtr pkt) override {
          return owner->innerMemPortRecvTimingSnoopResp(pkt);
        }
        void recvFunctional(PacketPtr pkt) override {
          return owner->innerMemPortRecvFunctional(pkt);
        }
        Tick recvAtomic(PacketPtr pkt) override {
          return owner->innerMemPortRecvAtomic(pkt);
        }
        void recvRespRetry() override {
          return owner->innerMemPortRecvRespRetry();
        }
        AddrRangeList getAddrRanges() const override {
          return owner->innerMemPortGetAddrRanges();
        }
    };

    friend class CPUSidePort;
    friend class MemSidePort;
    friend class InnerCPUSidePort;
    friend class InnerMemSidePort;

  protected:
    CPUSidePort cpu_side_port;
    MemSidePort mem_side_port;
    InnerCPUSidePort inner_cpu_port;
    InnerMemSidePort inner_mem_port;

    // CPU side port methods
    virtual bool cpuSidePortRecvTimingReq(PacketPtr pkt);
    virtual bool cpuSidePortRecvTimingSnoopResp(PacketPtr pkt);
    virtual void cpuSidePortRecvFunctional(PacketPtr pkt);
    virtual Tick cpuSidePortRecvAtomic(PacketPtr pkt);
    virtual void cpuSidePortRecvRespRetry();
    virtual AddrRangeList cpuSidePortGetAddrRanges() const;

    // Mem side port methods
    virtual bool memSidePortRecvTimingResp(PacketPtr pkt);
    virtual void memSidePortRecvReqRetry();
    virtual void memSidePortRecvTimingSnoopReq(PacketPtr pkt);
    virtual void memSidePortRecvRangeChange();

    // Inner CPU side port methods
    virtual bool innerCpuPortRecvTimingResp(PacketPtr pkt);
    virtual void innerCpuPortRecvReqRetry();
    virtual void innerCpuPortRecvTimingSnoopReq(PacketPtr pkt);
    virtual void innerCpuPortRecvRangeChange();

    // Inner Mem side port methods
    virtual bool innerMemPortRecvTimingReq(PacketPtr pkt);
    virtual bool innerMemPortRecvTimingSnoopResp(PacketPtr pkt);
    virtual void innerMemPortRecvFunctional(PacketPtr pkt);
    virtual Tick innerMemPortRecvAtomic(PacketPtr pkt);
    virtual void innerMemPortRecvRespRetry();
    virtual AddrRangeList innerMemPortGetAddrRanges() const;

  public:
    CacheWrapper(const CacheWrapperParams &p);

    Port &getPort(const std::string &if_name, PortID idx = InvalidPortID) override;
};

} // namespace gem5

#endif // __MEM_CACHE_CACHE_WRAPPER_HH__
