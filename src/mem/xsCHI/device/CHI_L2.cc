#include "mem/xsCHI/device/CHI_L2.hh"

#include <sys/types.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "base/addr_range.hh"
#include "base/compiler.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/CHIL2Wrapper.hh"
#include "mem/packet.hh"
#include "mem/xsCHI/base/FlitOpType.hh"
#include "params/Bridge.hh"
#include "params/ClockedObject.hh"
#include "params/SimObject.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
namespace xsCHI
{
namespace
{

Addr
blockAddrForDebug(Addr addr)
{
    return addr & ~static_cast<Addr>(0x3f);
}

} // namespace

    CHI_L2::WrapperStats::WrapperStats(CHI_L2 *parent)
        : statistics::Group(parent, "addr_observe"),
          ADD_STAT(observed_req_count, statistics::units::Count::get(),
                   "Observed cacheable requests entering xsCHI CHI_L2"),
          ADD_STAT(observed_req_addr_min, statistics::units::Byte::get(),
                   "Minimum observed request address (raw byte address)"),
          ADD_STAT(observed_req_addr_max, statistics::units::Byte::get(),
                   "Maximum observed request address (raw byte address)"),
          ADD_STAT(observed_req_addr_span, statistics::units::Byte::get(),
                   "Address span computed as max-min for observed requests"),
          ADD_STAT(shadow_mirror_req_total, statistics::units::Count::get(),
                   "Total mirrored requests sent to shadow bridges"),
          ADD_STAT(shadow_mirror_req_by_bridge, statistics::units::Count::get(),
                   "Mirrored requests per shadow bridge index"),
          ADD_STAT(shadow_remap_fail_total, statistics::units::Count::get(),
                   "Total shadow address remap validation failures"),
          ADD_STAT(shadow_remap_fail_by_bridge, statistics::units::Count::get(),
                   "Shadow address remap failures per bridge index"),
          ADD_STAT(shadow_drop_read_resp_total, statistics::units::Count::get(),
                   "Total read responses dropped from shadow bridges"),
          ADD_STAT(shadow_drop_read_resp_by_bridge, statistics::units::Count::get(),
                   "Dropped read responses per shadow bridge index")
    {
        using namespace statistics;
        // 统计向量维度直接绑定影子桥数量，确保 stats 中每个下标都可映射到具体 shadow[i]。
        const size_t shadowCount = parent->shadowBridges.size();
        // statistics::Vector does not accept zero-sized storage.
        // Keep a single disabled bucket when shadow is not configured.
        const size_t shadowStatDim = std::max<size_t>(shadowCount, 1);
        observed_req_count.flags(nozero);
        observed_req_addr_span.flags(nozero);
        shadow_mirror_req_total.flags(nozero);
        shadow_remap_fail_total.flags(nozero);
        shadow_drop_read_resp_total.flags(nozero);

        shadow_mirror_req_by_bridge.init(shadowStatDim).flags(nozero);
        shadow_remap_fail_by_bridge.init(shadowStatDim).flags(nozero);
        shadow_drop_read_resp_by_bridge.init(shadowStatDim).flags(nozero);

        if (shadowCount == 0) {
            shadow_mirror_req_by_bridge.subname(0, "disabled");
            shadow_remap_fail_by_bridge.subname(0, "disabled");
            shadow_drop_read_resp_by_bridge.subname(0, "disabled");
            return;
        }

        for (size_t i = 0; i < shadowCount; ++i) {
            // 统一命名为 shadow0/shadow1/...，便于在 stats.txt 中快速关联。
            const std::string label = "shadow" + std::to_string(i);
            shadow_mirror_req_by_bridge.subname(i, label);
            shadow_remap_fail_by_bridge.subname(i, label);
            shadow_drop_read_resp_by_bridge.subname(i, label);
        }
    }

    CHI_L2::CHI_L2(const Params &p):
    ClockedObject(p),
    cpuSidePort(p.name + ".cpu_side_port", this, "CpuSidePort"),
    memSidePort(p.name + ".mem_side_port", this, "MemSidePort"),
    bridge(p.RNBridge),
    shadowBridges(p.ShadowRNBridges.begin(), p.ShadowRNBridges.end()),
    shadowEnabled(p.shadow_enable),
    shadowSrcBases(p.shadow_src_bases.begin(), p.shadow_src_bases.end()),
    shadowWindowSizes(p.shadow_window_sizes.begin(), p.shadow_window_sizes.end()),
    shadowDstBases(p.shadow_dst_bases.begin(), p.shadow_dst_bases.end()),
    stats(this),
    observedMinAddr(0),
    observedMaxAddr(0),
    hasObservedAddr(false),
    observedReqCount(0)
    {
        bridge->set_recvReadResp_callback([this](ReqPtr& req) { this->recvReadResp(req); });
        // 严格失败策略（配置层 + C++ 双重把关）：
        // - 开启影子时，桥与三组映射参数都必须按索引对齐；
        // - 关闭影子时，不接受任何残留 shadow 参数，避免误配静默生效。
        if (shadowEnabled) {
            panic_if(shadowBridges.empty(),
                     "%s shadow_l2_enable is true but ShadowRNBridges is empty",
                     name());
            panic_if(shadowBridges.size() != shadowSrcBases.size() ||
                         shadowBridges.size() != shadowWindowSizes.size() ||
                         shadowBridges.size() != shadowDstBases.size(),
                     "%s shadow config length mismatch: bridges=%zu src=%zu window=%zu dst=%zu",
                     name(), shadowBridges.size(), shadowSrcBases.size(),
                     shadowWindowSizes.size(), shadowDstBases.size());
        } else {
            panic_if(!shadowBridges.empty() || !shadowSrcBases.empty() ||
                         !shadowWindowSizes.empty() || !shadowDstBases.empty(),
                     "%s shadow params provided while shadow_l2_enable is false",
                     name());
        }

        for (size_t i = 0; i < shadowBridges.size(); ++i) {
            CHIBridge *shadowBridge = shadowBridges[i];
            panic_if(shadowBridge == nullptr,
                     "%s ShadowRNBridges[%zu] is null", name(), i);
            // 影子读回包不参与功能正确性，只用于流量闭环，统一走丢弃回调。
            shadowBridge->set_recvReadResp_callback(
                [this, i](ReqPtr &req) { this->recvShadowReadResp(i, req); });
            // 队列解阻塞依赖“事务完成”回调（读/写都触发）。
            shadowBridge->set_txnComplete_callback(
                [this, i](ReqPtr &req) { this->onShadowTxnComplete(i, req); });
        }

        shadowReqQueues.resize(shadowBridges.size());
        shadowOutstandingByAddr.resize(shadowBridges.size());
        shadowQueueBlocked.assign(shadowBridges.size(), false);
        shadowReqSendEvents.resize(shadowBridges.size());
        shadowWriteCompletionSchedule.resize(shadowBridges.size());
        shadowWriteCompleteEvents.resize(shadowBridges.size());
        shadowReadIssueTickByAddr.resize(shadowBridges.size());
        shadowRecentReadLatencyCycles.resize(shadowBridges.size());
        shadowRecentReadLatencyCycleSums.assign(shadowBridges.size(), 0);
        shadowWriteAutoCompleteCycles.assign(
            shadowBridges.size(), ShadowWriteAutoCompleteCyclesDefault);
        for (size_t i = 0; i < shadowBridges.size(); ++i) {
            shadowReqSendEvents[i] = std::make_unique<EventFunctionWrapper>(
                [this, i]() { this->drainShadowReqQueue(i); },
                csprintf("%s.shadow_req_send[%zu]", name(), i));
            shadowWriteCompleteEvents[i] = std::make_unique<EventFunctionWrapper>(
                [this, i]() { this->processShadowWriteAutoComplete(i); },
                csprintf("%s.shadow_write_complete[%zu]", name(), i));
        }

        registerExitCallback([this]() {
            if (!hasObservedAddr) {
                inform("xsCHI %s observed no cacheable CHI requests", name());
                return;
            }

            inform("xsCHI %s observed_addr_range: count=%llu min=%#llx max=%#llx span=%#llx",
                   name(),
                   static_cast<unsigned long long>(observedReqCount),
                   static_cast<unsigned long long>(observedMinAddr),
                   static_cast<unsigned long long>(observedMaxAddr),
                   static_cast<unsigned long long>(observedMaxAddr - observedMinAddr));
        });
        DPRINTF(CHIL2Wrapper,"CHI_L2 Construct,without id\n");

    }

    void
    CHI_L2::init()
    {
        ClockedObject::init();
        // 启动阶段做窗口合法性校验，尽早失败，避免带着错误映射进入长仿真。
        std::vector<Addr> shadowDstLimits(shadowBridges.size(), 0);
        for (size_t i = 0; i < shadowBridges.size(); ++i) {
            const Addr srcBase = shadowSrcBases[i];
            const Addr winSize = shadowWindowSizes[i];
            const Addr dstBase = shadowDstBases[i];
            panic_if(winSize == 0,
                     "%s shadow[%zu] window size must be > 0", name(), i);
            panic_if(srcBase > std::numeric_limits<Addr>::max() - winSize,
                     "%s shadow[%zu] source window overflow: src=%#llx size=%#llx",
                     name(), i,
                     static_cast<unsigned long long>(srcBase),
                     static_cast<unsigned long long>(winSize));
            panic_if(dstBase > std::numeric_limits<Addr>::max() - winSize,
                     "%s shadow[%zu] destination window overflow: dst=%#llx size=%#llx",
                     name(), i,
                     static_cast<unsigned long long>(dstBase),
                     static_cast<unsigned long long>(winSize));
            const Addr srcLimit = srcBase + winSize;
            const Addr dstLimit = dstBase + winSize;
            shadowDstLimits[i] = dstLimit;
            inform("xsCHI %s shadow[%zu] remap: src=[%#llx,%#llx) dst=[%#llx,%#llx)",
                   name(), i,
                   static_cast<unsigned long long>(srcBase),
                   static_cast<unsigned long long>(srcLimit),
                   static_cast<unsigned long long>(dstBase),
                   static_cast<unsigned long long>(dstLimit));
        }

        // 防御式兜底：显式保证各 shadow 目标窗口互不重叠。
        for (size_t i = 0; i < shadowBridges.size(); ++i) {
            for (size_t j = i + 1; j < shadowBridges.size(); ++j) {
                const bool overlap =
                    (shadowDstBases[i] < shadowDstLimits[j]) &&
                    (shadowDstBases[j] < shadowDstLimits[i]);
                panic_if(overlap,
                         "%s shadow dst windows overlap: shadow[%zu]=[%#llx,%#llx) "
                         "shadow[%zu]=[%#llx,%#llx)",
                         name(),
                         i,
                         static_cast<unsigned long long>(shadowDstBases[i]),
                         static_cast<unsigned long long>(shadowDstLimits[i]),
                         j,
                         static_cast<unsigned long long>(shadowDstBases[j]),
                         static_cast<unsigned long long>(shadowDstLimits[j]));
            }
        }

        // Propagate address ranges so upstream crossbars have valid routing
        // before the first packet arrives.
        cpuSidePort.sendRangeChange();
    }
    // CHI_L2::CHI_L2(const Params &p,NodeID id,SystemAddressMap* sam):
    // ClockedObject(p),
    // cpuSidePort(p.name + ".cpu_side_port", this, "CpuSidePort"),
    // bridge(p,id,sam)
    // {
    //     bridge->set_recvReadResp_callback([this](ReqPtr& req) { this->recvReadResp(req); });
    //     DPRINTF(CHIL2Wrapper,"CHI_L2 Construct,id:%d",id.getNodeID());

    // }
    bool
    CHI_L2::CpuSidePort::recvTimingSnoopResp(PacketPtr pkt)
    {
        //todo:handle snoop situation！
        return true;
    }


    bool
    CHI_L2::CpuSidePort::tryTiming(PacketPtr pkt)
    {
        //no need to do it
        return true;
    }

    bool
    CHI_L2::CpuSidePort::recvTimingReq(PacketPtr pkt)
    {
        // if pkt is a Uncached request, we should redirect it to MemSidePort
        if (pkt->req->isUncacheable()) {
            DPRINTF(CHIL2Wrapper,"Recv Uncached request, redirect to MemSidePort, cmd:%s, addr: %lx\n",
                    pkt->cmdString(), pkt->getAddr());
            // redirect to MemSidePort
            return wrapper->memSidePort.sendTimingReq(pkt);
        }
        
        assert(pkt->isRequest());
        const unsigned mapSizeBefore = wrapper->outstanding_pkts.size();
        if (pkt->req && pkt->req->hasPC()) {
            DPRINTF(CHIL2Wrapper,
                    "cpu_req_track stage=recvTimingReq cmd=%s addr=%#lx "
                    "blk=%#lx pc=%#lx map_size_before=%u tick=%llu\n",
                    pkt->cmdString(),
                    pkt->getAddr(),
                    blockAddrForDebug(pkt->getAddr()),
                    pkt->req->getPC(),
                    mapSizeBefore,
                    static_cast<unsigned long long>(curTick()));
        } else {
            DPRINTF(CHIL2Wrapper,
                    "cpu_req_track stage=recvTimingReq cmd=%s addr=%#lx "
                    "blk=%#lx pc=NA map_size_before=%u tick=%llu\n",
                    pkt->cmdString(),
                    pkt->getAddr(),
                    blockAddrForDebug(pkt->getAddr()),
                    mapSizeBefore,
                    static_cast<unsigned long long>(curTick()));
        }
        wrapper->recordObservedAddress(pkt->getAddr());
        ReqPtr req = wrapper->CreateRequest(pkt);

        // 先镜像再发送主请求：
        // 这样在主路径出现后续 backpressure/时序变化时，影子与主请求仍保持同源同拍注入。
        wrapper->mirrorReqToShadows(req);
        wrapper->bridge->ReceiveReq(req, false);
        if (pkt->needsResponse() && !pkt->cacheResponding()) {
            if (wrapper->outstanding_pkts.count(pkt->getAddr()) != 0) {
                wrapper->dumpOutstandingPkts("recvTimingReq_duplicate_addr",
                                            pkt->getAddr());
            }
            assert(wrapper->outstanding_pkts.count(pkt->getAddr())==0);
            assert(!pkt->isWrite());
            wrapper->outstanding_pkts[pkt->getAddr()] = pkt;
            DPRINTF(CHIL2Wrapper,
                    "cpu_req_track stage=outstanding_insert cmd=%s "
                    "addr=%#lx blk=%#lx map_size_before=%u "
                    "map_size_after=%u tick=%llu\n",
                    pkt->cmdString(),
                    pkt->getAddr(),
                    blockAddrForDebug(pkt->getAddr()),
                    mapSizeBefore,
                    static_cast<unsigned>(wrapper->outstanding_pkts.size()),
                    static_cast<unsigned long long>(curTick()));
        }
        //always true
        return true;
    }

    void
    CHI_L2::recordObservedAddress(Addr addr)
    {
        if (!hasObservedAddr) {
            observedMinAddr = addr;
            observedMaxAddr = addr;
            hasObservedAddr = true;
        } else {
            observedMinAddr = std::min(observedMinAddr, addr);
            observedMaxAddr = std::max(observedMaxAddr, addr);
        }

        observedReqCount++;
        stats.observed_req_count++;
        stats.observed_req_addr_min = observedMinAddr;
        stats.observed_req_addr_max = observedMaxAddr;
        stats.observed_req_addr_span = observedMaxAddr - observedMinAddr;
    }

    Addr
    CHI_L2::remapShadowAddr(size_t shadowIdx, Addr addr)
    {
        const Addr srcBase = shadowSrcBases[shadowIdx];
        const Addr winSize = shadowWindowSizes[shadowIdx];
        const Addr dstBase = shadowDstBases[shadowIdx];
        Addr remappedAddr = 0;
        const bool ok = TestApi::remapAddressInWindow(
            addr, srcBase, winSize, dstBase, remappedAddr);
        if (!ok) {
            // 记统计后立即 panic：映射失败意味着地址隔离假设被破坏，实验结果不可用。
            stats.shadow_remap_fail_total++;
            stats.shadow_remap_fail_by_bridge[shadowIdx]++;
            panic("%s shadow[%zu] address %#llx outside source window [%#llx, %#llx)",
                  name(), shadowIdx,
                  static_cast<unsigned long long>(addr),
                  static_cast<unsigned long long>(srcBase),
                  static_cast<unsigned long long>(srcBase + winSize));
        }
        return remappedAddr;
    }

    void
    CHI_L2::mirrorReqToShadows(const ReqPtr &req)
    {
        if (!shadowEnabled) {
            return;
        }

        for (size_t i = 0; i < shadowBridges.size(); ++i) {
            // 深拷贝 Request，保证每个影子拥有独立地址字段与后续生命周期。
            ReqPtr shadowReq = std::make_shared<Request>(*req);
            const Addr shadowAddr = remapShadowAddr(i, req->getAddr());
            shadowReq->setAddr(shadowAddr);
                shadowReqQueues[i].push_back(shadowReq);
                scheduleShadowReqSend(i);
            stats.shadow_mirror_req_total++;
            stats.shadow_mirror_req_by_bridge[i]++;
            DPRINTF(CHIL2Wrapper,
                    "MirrorReq shadow[%zu] op:%s addr:%#llx -> %#llx\n",
                    i,
                    CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(shadowReq->getOpcode()),
                    static_cast<unsigned long long>(req->getAddr()),
                    static_cast<unsigned long long>(shadowAddr));
        }
    }

    void
    CHI_L2::scheduleShadowReqSend(size_t shadowIdx)
    {
        panic_if(shadowIdx >= shadowReqSendEvents.size(),
                 "%s scheduleShadowReqSend index out of range: %zu",
                 name(), shadowIdx);
        auto &event = shadowReqSendEvents[shadowIdx];
        if (event && !event->scheduled()) {
            schedule(*event, clockEdge(Cycles(1)));
        }
    }

    void
    CHI_L2::drainShadowReqQueue(size_t shadowIdx)
    {
        panic_if(shadowIdx >= shadowReqQueues.size() ||
                     shadowIdx >= shadowOutstandingByAddr.size() ||
                     shadowIdx >= shadowBridges.size(),
                 "%s drainShadowReqQueue index out of range: %zu",
                 name(), shadowIdx);

        auto &queue = shadowReqQueues[shadowIdx];
        if (queue.empty()) {
            shadowQueueBlocked[shadowIdx] = false;
            return;
        }

        ReqPtr req = queue.front();
        panic_if(!req, "%s shadow[%zu] queue front request is null", name(), shadowIdx);
        const Addr addr = req->getAddr();
        auto &outstanding = shadowOutstandingByAddr[shadowIdx];
        const bool trackOutstanding = shadowNeedOutstandingTrack(req);

        if (outstanding.count(addr) > 0) {
            shadowQueueBlocked[shadowIdx] = true;
            DPRINTF(CHIL2Wrapper,
                    "ShadowReqQueue blocked shadow[%zu] op:%s addr:%#llx queue=%zu outstanding_same_addr=%u\n",
                    shadowIdx,
                    CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(req->getOpcode()),
                    static_cast<unsigned long long>(addr),
                    queue.size(),
                    outstanding[addr]);
            return;
        }

        // CHIBridge::ReceiveReq(req,false) 在发送失败场景会自己入桥内重试队列，
        // 这里无论返回值都视为已由 bridge 接管，避免重复注入同一请求。
        shadowBridges[shadowIdx]->ReceiveReq(req, false);
        queue.pop_front();
        if (trackOutstanding) {
            outstanding[addr]++;
            if (shadowNeedReadLatencySample(req)) {
                recordShadowReadIssue(shadowIdx, addr);
            }
            if (shadowNeedAutoWriteComplete(req)) {
                scheduleShadowWriteAutoComplete(shadowIdx, addr);
            }
        }
        shadowQueueBlocked[shadowIdx] = false;
        DPRINTF(CHIL2Wrapper,
                "ShadowReqQueue handoff shadow[%zu] op:%s addr:%#llx queue=%zu outstanding_same_addr=%u\n",
                shadowIdx,
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(req->getOpcode()),
                static_cast<unsigned long long>(addr),
                queue.size(),
            trackOutstanding ? outstanding[addr] : 0);

        if (!queue.empty()) {
            scheduleShadowReqSend(shadowIdx);
        }
    }

    void
    CHI_L2::onShadowTxnComplete(size_t shadowIdx, ReqPtr &req)
    {
        panic_if(shadowIdx >= shadowOutstandingByAddr.size() ||
                     shadowIdx >= shadowQueueBlocked.size(),
                 "%s onShadowTxnComplete index out of range: %zu",
                 name(), shadowIdx);
        panic_if(!req, "%s shadow[%zu] completion request is null", name(), shadowIdx);

        // 写回类请求使用固定周期超时自动完成，避免双重记账。
        if (shadowNeedAutoWriteComplete(req)) {
            return;
        }

        // 读类请求通过真实完成回调驱动 outstanding 释放。
        if (!shadowNeedOutstandingTrack(req)) {
            return;
        }

        const Addr addr = req->getAddr();
        if (shadowNeedReadLatencySample(req)) {
            recordShadowReadCompletion(shadowIdx, addr);
        }
        auto &outstanding = shadowOutstandingByAddr[shadowIdx];
        auto it = outstanding.find(addr);
        if (it == outstanding.end()) {
            warn("%s shadow[%zu] completion for untracked addr=%#llx",
                 name(), shadowIdx,
                 static_cast<unsigned long long>(addr));
        } else {
            assert(it->second > 0);
            it->second--;
            if (it->second == 0) {
                outstanding.erase(it);
            }
        }

        // 阻塞队列仅在收到响应时检查一次可否解阻。
        if (!shadowReqQueues[shadowIdx].empty() && shadowQueueBlocked[shadowIdx]) {
            const Addr headAddr = shadowReqQueues[shadowIdx].front()->getAddr();
            if (outstanding.count(headAddr) == 0) {
                shadowQueueBlocked[shadowIdx] = false;
                scheduleShadowReqSend(shadowIdx);
            }
        }
    }

    bool
    CHI_L2::shadowNeedOutstandingTrack(const ReqPtr &req) const
    {
        panic_if(!req, "%s shadowNeedOutstandingTrack got null req", name());
        const CHI_OP_TYPE op = req->getOpcode();
        return op == CHI_OP_TYPE::CHI_REQ_READUNIQUE ||
               op == CHI_OP_TYPE::CHI_REQ_READSHARED ||
               op == CHI_OP_TYPE::CHI_REQ_READCLEAN ||
               op == CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL ||
               op == CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL ||
               (op == CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE && !req->getCacheResponding());
    }

    bool
    CHI_L2::shadowNeedAutoWriteComplete(const ReqPtr &req) const
    {
        panic_if(!req, "%s shadowNeedAutoWriteComplete got null req", name());
        const CHI_OP_TYPE op = req->getOpcode();
        return op == CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL ||
               op == CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL;
    }

    bool
    CHI_L2::shadowNeedReadLatencySample(const ReqPtr &req) const
    {
        panic_if(!req, "%s shadowNeedReadLatencySample got null req", name());
        if (shadowNeedAutoWriteComplete(req)) {
            return false;
        }
        return shadowNeedOutstandingTrack(req);
    }

    void
    CHI_L2::recordShadowReadIssue(size_t shadowIdx, Addr addr)
    {
        panic_if(shadowIdx >= shadowReadIssueTickByAddr.size(),
                 "%s recordShadowReadIssue index out of range: %zu",
                 name(), shadowIdx);
        shadowReadIssueTickByAddr[shadowIdx][addr] = curTick();
    }

    void
    CHI_L2::recordShadowReadCompletion(size_t shadowIdx, Addr addr)
    {
        panic_if(shadowIdx >= shadowReadIssueTickByAddr.size() ||
                     shadowIdx >= shadowRecentReadLatencyCycles.size() ||
                     shadowIdx >= shadowRecentReadLatencyCycleSums.size() ||
                     shadowIdx >= shadowWriteAutoCompleteCycles.size(),
                 "%s recordShadowReadCompletion index out of range: %zu",
                 name(), shadowIdx);

        auto &issueMap = shadowReadIssueTickByAddr[shadowIdx];
        auto issueIt = issueMap.find(addr);
        if (issueIt == issueMap.end()) {
            return;
        }

        const Tick issueTick = issueIt->second;
        issueMap.erase(issueIt);
        if (curTick() <= issueTick || clockPeriod() == 0) {
            return;
        }

        const Tick latencyTicks = curTick() - issueTick;
        const uint32_t latencyCycles = std::max<uint32_t>(
            1, static_cast<uint32_t>((latencyTicks + clockPeriod() - 1) /
                                     clockPeriod()));

        auto &window = shadowRecentReadLatencyCycles[shadowIdx];
        auto &sum = shadowRecentReadLatencyCycleSums[shadowIdx];
        window.push_back(latencyCycles);
        sum += latencyCycles;
        if (window.size() > ShadowReadLatencyWindow) {
            sum -= window.front();
            window.pop_front();
        }

        const uint32_t avgCycles = static_cast<uint32_t>(
            std::max<uint64_t>(1, sum / window.size()));
        shadowWriteAutoCompleteCycles[shadowIdx] = avgCycles;
    }

    void
    CHI_L2::scheduleShadowWriteAutoComplete(size_t shadowIdx, Addr addr)
    {
        panic_if(shadowIdx >= shadowWriteCompletionSchedule.size() ||
                     shadowIdx >= shadowWriteCompleteEvents.size(),
                 "%s scheduleShadowWriteAutoComplete index out of range: %zu",
                 name(), shadowIdx);

        panic_if(shadowIdx >= shadowWriteAutoCompleteCycles.size(),
                 "%s scheduleShadowWriteAutoComplete missing cycle config: %zu",
                 name(), shadowIdx);
        const Tick due = curTick() +
            clockPeriod() * std::max<uint32_t>(1, shadowWriteAutoCompleteCycles[shadowIdx]);
        shadowWriteCompletionSchedule[shadowIdx].emplace(due, addr);
        scheduleNextShadowWriteCompleteEvent(shadowIdx);
    }

    void
    CHI_L2::scheduleNextShadowWriteCompleteEvent(size_t shadowIdx)
    {
        panic_if(shadowIdx >= shadowWriteCompletionSchedule.size() ||
                     shadowIdx >= shadowWriteCompleteEvents.size(),
                 "%s scheduleNextShadowWriteCompleteEvent index out of range: %zu",
                 name(), shadowIdx);

        auto &scheduleMap = shadowWriteCompletionSchedule[shadowIdx];
        auto &event = shadowWriteCompleteEvents[shadowIdx];
        if (scheduleMap.empty() || !event) {
            return;
        }

        const Tick nextDue = scheduleMap.begin()->first;
        if (event->scheduled()) {
            if (event->when() <= nextDue) {
                return;
            }
            deschedule(*event);
        }
        schedule(*event, nextDue);
    }

    void
    CHI_L2::processShadowWriteAutoComplete(size_t shadowIdx)
    {
        panic_if(shadowIdx >= shadowWriteCompletionSchedule.size() ||
                     shadowIdx >= shadowOutstandingByAddr.size() ||
                     shadowIdx >= shadowQueueBlocked.size() ||
                     shadowIdx >= shadowReqQueues.size(),
                 "%s processShadowWriteAutoComplete index out of range: %zu",
                 name(), shadowIdx);

        auto &scheduleMap = shadowWriteCompletionSchedule[shadowIdx];
        auto &outstanding = shadowOutstandingByAddr[shadowIdx];

        while (!scheduleMap.empty() && scheduleMap.begin()->first <= curTick()) {
            const Addr addr = scheduleMap.begin()->second;
            scheduleMap.erase(scheduleMap.begin());

            auto it = outstanding.find(addr);
            if (it == outstanding.end()) {
                continue;
            }

            assert(it->second > 0);
            it->second--;
            if (it->second == 0) {
                outstanding.erase(it);
            }
        }

        if (!shadowReqQueues[shadowIdx].empty() && shadowQueueBlocked[shadowIdx]) {
            const Addr headAddr = shadowReqQueues[shadowIdx].front()->getAddr();
            if (outstanding.count(headAddr) == 0) {
                shadowQueueBlocked[shadowIdx] = false;
                scheduleShadowReqSend(shadowIdx);
            }
        }

        scheduleNextShadowWriteCompleteEvent(shadowIdx);
    }



    // AddrRangeList
    // CHI_L2::CpuSidePort::getAddrRanges() const
    // {
    //     return cache->getAddrRanges();
    // }


    CHI_L2::
    CpuSidePort::CpuSidePort(const std::string &_name, CHI_L2 *wrapper,
                            const std::string &_label)
        : CacheResponsePort(_name, wrapper, _label),wrapper(wrapper)
    {
    }

    CHI_L2::MemSidePort::MemSidePort(const std::string &_name,
                                        CHI_L2 *wrapper,
                                        const std::string &_label)
        : CacheRequestPort(_name, wrapper, _reqQueue, _snoopRespQueue),
        _reqQueue(*wrapper, *this, _label),
        _snoopRespQueue(*wrapper, *this, true, _label), wrapper(wrapper)
    {
    }

    CHI_L2::CacheResponsePort::CacheResponsePort(const std::string &_name,
                                            CHI_L2 *wrapper,
                                            const std::string &_label)
        : QueuedResponsePort(_name, wrapper, queue),
        queue(*wrapper, *this, true, _label),
        blocked(false), mustSendRetry(false),
        sendRetryEvent([this]{ processSendRetry(); }, _name)
    {
    }

    void
    CHI_L2::CacheResponsePort::setBlocked()
    {
        assert(!blocked);
        // DPRINTF(CHIL2Wrapper, "Port is blocking new requests\n");
        blocked = true;
        // if we already scheduled a retry in this cycle, but it has not yet
        // happened, cancel it
        if (sendRetryEvent.scheduled()) {
            owner.deschedule(sendRetryEvent);
            // DPRINTF(CHIL2Wrapper, "Port descheduled retry\n");
            mustSendRetry = true;
        }
    }

    void
    CHI_L2::CacheResponsePort::clearBlocked()
    {
        assert(blocked);
        // DPRINTF(CHIL2Wrapper, "Port is accepting new requests\n");
        blocked = false;
        if (mustSendRetry) {
            // @TODO: need to find a better time (next cycle?)
            owner.schedule(sendRetryEvent, curTick() + 1);
        }
    }

    void
    CHI_L2::CacheResponsePort::processSendRetry()
    {
        DPRINTF(CHIL2Wrapper, "Port is sending retry\n");

        // reset the flag and call retry
        mustSendRetry = false;
        sendRetryReq();
    }

    Tick
    CHI_L2::CpuSidePort::recvAtomic(PacketPtr pkt)
    {
        panic("not supported");
        return curTick();
    }

    void
    CHI_L2::CpuSidePort::recvFunctional(PacketPtr pkt)
    {
        panic("not supported");
    }

    AddrRangeList
    CHI_L2::CpuSidePort::getAddrRanges() const
    {
        AddrRangeList ranges;
        // Advertise a catch-all range so upstream crossbars know this port can
        // service any address that reaches the L2 wrapper.
        ranges.push_back(RangeSize(0, MaxAddr));
        return ranges;
    }

    ReqPtr
    CHI_L2::CreateRequest(PacketPtr pkt)
    {
        //phrase pkt
        Addr addr = pkt->getAddr();
        uint32_t size = pkt->getSize();
        CHI_OP_TYPE op = CHI_OP_TYPE::CHI_REQ_OP_START;
        bool pktHasData = false;
        if (pkt->cmd==MemCmd(MemCmd::ReadExReq)){
            op = CHI_OP_TYPE::CHI_REQ_READUNIQUE;
        }else if (pkt->cmd==MemCmd(MemCmd::ReadSharedReq)){
            op = CHI_OP_TYPE::CHI_REQ_READSHARED;
        }else if (pkt->cmd==MemCmd(MemCmd::ReadCleanReq)) {
            op = CHI_OP_TYPE::CHI_REQ_READCLEAN;
        }else if (pkt->cmd==MemCmd(MemCmd::CleanEvict)) {
            op = CHI_OP_TYPE::CHI_REQ_EVICT;
        }else if (pkt->cmd==MemCmd(MemCmd::WritebackDirty)) {
            op = CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL;
            pktHasData = true;
        }else if (pkt->cmd==MemCmd(MemCmd::WritebackClean)){
            op = CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL;
            pktHasData = true;
        }else if (pkt->cmd==MemCmd(MemCmd::HardPFReq)) {
            op = CHI_OP_TYPE::CHI_REQ_READUNIQUE;
        }else if (pkt->cmd==MemCmd(MemCmd::UpgradeReq)){
            op = CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE;
        }else {
            assert(false && "unsupported Req!");
        }
        DPRINTF(CHIL2Wrapper,"Create Req, op:%s, addr: %lx, size:%d\n",CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op),addr,size);
        ReqPtr req = std::make_shared<Request>(op,addr,size);
        req->setCacheResponding(pkt->cacheResponding());
        req->setResponderHadWritable(pkt->responderHadWritable());
        if (pktHasData) {
            req->setData(pkt);
        }
        return req;
    }

    void
    CHI_L2::dumpOutstandingPkts(const char *reason, Addr focusAddr) const
    {
        DPRINTF(CHIL2Wrapper,
                "outstanding_pkts_dump reason=%s focus=%#lx size=%u tick=%llu\n",
                reason,
                focusAddr,
                static_cast<unsigned>(outstanding_pkts.size()),
                static_cast<unsigned long long>(curTick()));
        for (const auto &[addr, pkt] : outstanding_pkts) {
            DPRINTF(CHIL2Wrapper,
                    "outstanding_pkts_dump addr=%#lx blk=%#lx cmd=%s pkt=%p needsResp=%d cacheResponding=%d\n",
                    addr,
                    blockAddrForDebug(addr),
                    pkt ? pkt->cmdString() : "null",
                    pkt,
                    pkt ? pkt->needsResponse() : 0,
                    pkt ? pkt->cacheResponding() : 0);
        }
    }

    void
    CHI_L2::recvReadResp(ReqPtr &req){
        const unsigned mapSizeBefore = outstanding_pkts.size();
        DPRINTF(CHIL2Wrapper,
                "recvReadResp stage=recv addr=%#lx blk=%#lx opcode=%s size=%u map_size_before=%u tick=%llu\n",
                req->getAddr(),
                blockAddrForDebug(req->getAddr()),
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(req->getOpcode()),
                req->getSize(),
                mapSizeBefore,
                static_cast<unsigned long long>(curTick()));
        if (outstanding_pkts.count(req->getAddr()) == 0) {
            dumpOutstandingPkts("recvReadResp_missing_addr", req->getAddr());
        }
        assert(outstanding_pkts.count(req->getAddr())>0);
        PacketPtr pkt = outstanding_pkts[req->getAddr()];
        assert(pkt->needsResponse());
        // todo: properly set delay!
        // decide to ignore the delay, leave it to l2xbar
        // assert(pkt->headerDelay == 0);
        // assert(pkt->payloadDelay == 0);
        pkt->makeTimingResponse();
        if (req->getOpcode() != CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE &&
            req->getOpcode() != CHI_OP_TYPE::CHI_REQ_EVICT)
        {
            uint8_t *tmp = new uint8_t[req->getSize()];
            assert(req->getSize()==pkt->getSize());
            req->getData(tmp);
            pkt->setData(tmp);
            delete[] tmp; // 释放临时内存
        }
        cpuSidePort.schedTimingResp(pkt, curTick());

        outstanding_pkts.erase(req->getAddr());
        DPRINTF(CHIL2Wrapper,
                "recvReadResp stage=erase addr=%#lx blk=%#lx opcode=%s "
                "size=%u map_size_before=%u map_size_after=%u tick=%llu\n",
                req->getAddr(),
                blockAddrForDebug(req->getAddr()),
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(req->getOpcode()),
                req->getSize(),
                mapSizeBefore,
                static_cast<unsigned>(outstanding_pkts.size()),
                static_cast<unsigned long long>(curTick()));

    }

    void
    CHI_L2::recvShadowReadResp(size_t shadowIdx, ReqPtr &req)
    {
        panic_if(shadowIdx >= shadowBridges.size(),
                 "%s shadow read callback index out of range: %zu",
                 name(), shadowIdx);
        stats.shadow_drop_read_resp_total++;
        stats.shadow_drop_read_resp_by_bridge[shadowIdx]++;
        // 不向 CPU 回传影子响应：
        // 影子仅用于制造网络负载，功能语义以主桥回包为准。
        DPRINTF(CHIL2Wrapper,
                "DropShadowReadResp shadow[%zu], op:%s, addr:%#llx, size:%u\n",
                shadowIdx,
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(req->getOpcode()),
                static_cast<unsigned long long>(req->getAddr()),
                req->getSize());
    }
    gem5::Port &
    CHI_L2::getPort(const std::string &if_name, PortID idx)
    {

        if (if_name == "mem_side_port")
            return memSidePort;
        else if (if_name == "cpu_side_port")
            return cpuSidePort;
        else
            // pass it along to our super class
            return ClockedObject::getPort(if_name, idx);
    }
    CHIPort*
    CHI_L2::getCHIPort(){
        return bridge->getNetworkPort();
    }
    CHIBridge* CHI_L2::getBridge(){
        return bridge;
    }

    ///////////////
//
// MemSidePort
//
///////////////
bool
CHI_L2::MemSidePort::recvTimingResp(PacketPtr pkt)
{
    wrapper->cpuSidePort.schedTimingResp(pkt, curTick());
    // cache->recvTimingResp(pkt);
    return true;
}

void
CHI_L2::MemSidePort::recvFunctionalCustomSignal(PacketPtr pkt, int sig)
{
    assert(false && "recvFunctionalCustomSignal not implemented in CHI_L2::MemSidePort");
}

// Express snooping requests to memside port
void
CHI_L2::MemSidePort::recvTimingSnoopReq(PacketPtr pkt)
{
    // Snoops shouldn't happen when bypassing caches
    assert(false && "Snoops should not happen in CHI_L2");

}

Tick
CHI_L2::MemSidePort::recvAtomicSnoop(PacketPtr pkt)
{
    panic("not supported");
    return curTick();
}

void
CHI_L2::MemSidePort::recvFunctionalSnoop(PacketPtr pkt)
{
    panic("not supported");
}
}
}
