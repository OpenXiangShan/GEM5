#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "debug/CHIL2Wrapper.hh"
#include "debug/CHIPort.hh"
#include "mem/abstract_mem.hh"
#include "mem/packet.hh"
#include "mem/qport.hh"
#include "mem/xsCHI/base/CHIPort.hh"
#include "mem/xsCHI/base/module.hh"
#include "mem/xsCHI/base/request.hh"
#include "mem/xsCHI/device/CHIBridge.hh"
#include "params/ClockedObject.hh"
#include "params/L2ToDramSys.hh"
#include "params/CHI_L2.hh"
#include "params/SimObject.hh"
#include "sim/eventq.hh"
#include "sim/stats.hh"

namespace gem5 {
namespace xsCHI {
    class CHI_L2 : public ClockedObject
    {
      // CHI_L2's job : when recv a pkt from Gem5Cache,
      // it will convert the pkt to a xsCHI request, and send it to CHIBridge port.
      // when recv a xsCHI request from CHIBridge port, the request can only be a snoop request,
      // it will convert the xsCHI request to a pkt,
      // and send it to back Gem5Cache.(which currently is not considered, we do not support snoop yet.)
    public:
    class TestApi
    {
      public:
        // 仅用于单元测试与纯函数验证：
        // 在不依赖完整仿真对象的情况下，验证影子地址重映射边界。
        static bool remapAddressInWindow(Addr addr, Addr srcBase, Addr windowSize,
                                         Addr dstBase, Addr &remappedAddr);
    };

    protected:
     /**
     * A cache response port is used for the CPU-side port of the cache,
     * and it is basically a simple timing port that uses a transmit
     * list for responses to the CPU (or connected requestor). In
     * addition, it has the functionality to block the port for
     * incoming requests. If blocked, the port will issue a retry once
     * unblocked.
     */
    class CacheResponsePort : public QueuedResponsePort
    {

      public:

        /** Do not accept any new requests. */
        void setBlocked();

        /** Return to normal operation and accept new requests. */
        void clearBlocked();

        bool isBlocked() const { return blocked; }

      protected:
        CacheResponsePort(const std::string &_name, CHI_L2 *wrapper,
                       const std::string &_label);

        /** A normal packet queue used to store responses. */
        RespPacketQueue queue;

        bool blocked;

        bool mustSendRetry;

        void processSendRetry();

        EventFunctionWrapper sendRetryEvent;

    };
     /**
     * The CPU-side port extends the base cache response port with access
     * functions for functional, atomic and timing requests.
     */
    class CpuSidePort : public CacheResponsePort
    {
      private:

        // // a pointer to our specific cache implementation
        // Module *cache;
        CHI_L2 *wrapper;

      protected:
        virtual bool recvTimingSnoopResp(PacketPtr pkt) override;

        virtual bool tryTiming(PacketPtr pkt) override;

        virtual bool recvTimingReq(PacketPtr pkt) override;

        virtual Tick recvAtomic(PacketPtr pkt) override;

        virtual void recvFunctional(PacketPtr pkt) override;

        virtual AddrRangeList getAddrRanges() const override;

      public:

        CpuSidePort(const std::string &_name, CHI_L2 *wrapper,
                    const std::string &_label);

    };
    class CacheRequestPort : public QueuedRequestPort
    {

      public:

        /**
         * Schedule a send of a request packet (from the MSHR). Note
         * that we could already have a retry outstanding.
         */
        void schedSendEvent(Tick time)
        {
            DPRINTF(CHIL2Wrapper, "Scheduling send event at %llu\n", time);
            reqQueue.schedSendEvent(time);
        }

      protected:

        CacheRequestPort(const std::string &_name, CHI_L2 *wrapper,
                        ReqPacketQueue &_reqQueue,
                        SnoopRespPacketQueue &_snoopRespQueue) :
            QueuedRequestPort(_name, wrapper, _reqQueue, _snoopRespQueue)
        { }

        /**
         * Memory-side port always snoops.
         *
         * @return always true
         */
        virtual bool isSnooping() const { return true; }
    };

    class MemSidePort : public CacheRequestPort
    {
      private:

        /** The cache-specific queue. */
        ReqPacketQueue _reqQueue;

        SnoopRespPacketQueue _snoopRespQueue;

        // a pointer to our specific cache implementation
        CHI_L2 *wrapper;

      protected:

        virtual void recvTimingSnoopReq(PacketPtr pkt);

        virtual bool recvTimingResp(PacketPtr pkt);

        virtual Tick recvAtomicSnoop(PacketPtr pkt);

        virtual void recvFunctionalSnoop(PacketPtr pkt);

        virtual void recvFunctionalCustomSignal(PacketPtr pkt, int sig);

      public:

        MemSidePort(const std::string &_name, CHI_L2 *wrapper,
                    const std::string &_label);

        bool hasSchedSendEvent() const { return true; }
    };

    CpuSidePort cpuSidePort;//for recv origin request,and convert it to xsCHI request
    MemSidePort memSidePort;//for redirect Uncached requests to IO devices

    // extract command , data , address from pkt, and create a xsCHI request,other fields are ignored.
    // maybe we need to cache these pkts in case we need to send them back to Gem5Cache.
    ReqPtr  CreateRequest(PacketPtr pkt);

    CHIBridge* bridge; // the bridge to xsCHI network
    // 多影子桥列表：每个桥都作为独立 RN 流量源注入到 Mesh。
    std::vector<CHIBridge*> shadowBridges;
    // 总开关：关闭时 shadowBridges 与三组映射参数必须都为空（严格失败）。
    bool shadowEnabled;
    // 影子地址映射参数（按同一索引一一对应）：
    // 映射公式 A' = dst_base + (A - src_base)，要求 A ∈ [src_base, src_base+window_size)。
    std::vector<Addr> shadowSrcBases;
    std::vector<Addr> shadowWindowSizes;
    std::vector<Addr> shadowDstBases;
    void recvReadResp(ReqPtr &req);
    void recvShadowReadResp(size_t shadowIdx, ReqPtr &req);

    std::unordered_map<uint64_t, PacketPtr> outstanding_pkts;

    struct WrapperStats : public statistics::Group
    {
        explicit WrapperStats(CHI_L2 *parent);

        statistics::Scalar observed_req_count;
        statistics::Scalar observed_req_addr_min;
        statistics::Scalar observed_req_addr_max;
        statistics::Scalar observed_req_addr_span;

        // 影子相关统计：
        // - mirror_req: 成功注入到影子桥的请求数
        // - remap_fail: 映射校验失败次数（失败后会 panic，计数可辅助定位）
        // - drop_read_resp: 影子读回包丢弃次数（验证不会回 CPU）
        statistics::Scalar shadow_mirror_req_total;
        statistics::Vector shadow_mirror_req_by_bridge;
        statistics::Scalar shadow_remap_fail_total;
        statistics::Vector shadow_remap_fail_by_bridge;
        statistics::Scalar shadow_drop_read_resp_total;
        statistics::Vector shadow_drop_read_resp_by_bridge;
    } stats;

    Addr observedMinAddr;
    Addr observedMaxAddr;
    bool hasObservedAddr;
    uint64_t observedReqCount;

    void recordObservedAddress(Addr addr);
    // 对指定 shadow 索引执行地址映射；非法直接 panic（不允许静默降级）。
    Addr remapShadowAddr(size_t shadowIdx, Addr addr);
    // 将主请求复制并注入所有影子桥，形成 1+N 的流量源效果。
    void mirrorReqToShadows(const ReqPtr &req);
    void drainShadowReqQueue(size_t shadowIdx);
    void scheduleShadowReqSend(size_t shadowIdx);
    void onShadowTxnComplete(size_t shadowIdx, ReqPtr &req);
    bool shadowNeedOutstandingTrack(const ReqPtr &req) const;
    bool shadowNeedReadLatencySample(const ReqPtr &req) const;
    bool shadowNeedAutoWriteComplete(const ReqPtr &req) const;
    void scheduleShadowWriteAutoComplete(size_t shadowIdx, Addr addr);
    void processShadowWriteAutoComplete(size_t shadowIdx);
    void scheduleNextShadowWriteCompleteEvent(size_t shadowIdx);
    void recordShadowReadIssue(size_t shadowIdx, Addr addr);
    void recordShadowReadCompletion(size_t shadowIdx, Addr addr);

    std::vector<std::deque<ReqPtr>> shadowReqQueues;
    std::vector<std::unordered_map<Addr, unsigned>> shadowOutstandingByAddr;
    std::vector<bool> shadowQueueBlocked;
    std::vector<std::unique_ptr<EventFunctionWrapper>> shadowReqSendEvents;
    std::vector<std::multimap<Tick, Addr>> shadowWriteCompletionSchedule;
    std::vector<std::unique_ptr<EventFunctionWrapper>> shadowWriteCompleteEvents;
    std::vector<std::unordered_map<Addr, Tick>> shadowReadIssueTickByAddr;
    std::vector<std::deque<uint32_t>> shadowRecentReadLatencyCycles;
    std::vector<uint64_t> shadowRecentReadLatencyCycleSums;
    std::vector<uint32_t> shadowWriteAutoCompleteCycles;
    static constexpr uint32_t ShadowReadLatencyWindow = 100;
    static constexpr uint32_t ShadowWriteAutoCompleteCyclesDefault = 100;

    public:
    gem5::Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;

    // std::string name() const override{ return "CHI_L2"; }

    typedef CHI_L2Params Params;
    CHI_L2(const Params &p);
    // CHI_L2(const Params &p,NodeID id,SystemAddressMap* sam);
    CHI_L2();
    // ~CHI_L2() = default;
    CHIBridge* getBridge();
    CHIPort* getCHIPort();
    const std::vector<CHIBridge*> &getShadowBridges() const { return shadowBridges; }
    void setNodeID(uint32_t id){getBridge()->setNodeID(id);}
    void setSAM(std::shared_ptr<SystemAddressMapRN> sam){getBridge()->setSAM(sam);}
    void init() override;
  };

  inline bool
  CHI_L2::TestApi::remapAddressInWindow(Addr addr, Addr srcBase,
                                           Addr windowSize, Addr dstBase,
                                           Addr &remappedAddr)
  {
      // 1) 窗口大小必须非 0，否则映射区间无意义。
      if (windowSize == 0) {
          return false;
      }
      // 2) 源窗口上界计算前先防溢出，避免 srcBase + windowSize 回绕。
      if (srcBase > std::numeric_limits<Addr>::max() - windowSize) {
          return false;
      }
      const Addr srcLimit = srcBase + windowSize;
      // 3) 地址必须落在源窗口内（左闭右开）。
      if (addr < srcBase || addr >= srcLimit) {
          return false;
      }
      const Addr offset = addr - srcBase;
      // 4) 目标地址同样要防溢出，避免 dstBase + offset 回绕。
      if (dstBase > std::numeric_limits<Addr>::max() - offset) {
          return false;
      }
      // 5) 计算重映射地址。
      remappedAddr = dstBase + offset;
      return true;
  }
}
}
