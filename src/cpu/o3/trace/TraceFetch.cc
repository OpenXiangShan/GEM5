/*
 * Copyright (c) 2025
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/o3/trace/TraceFetch.hh"

#include <algorithm>
#include <array>
#include <limits>

#include "arch/riscv/isa.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/pcstate.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/fetch.hh"
#include "debug/Fetch.hh"
#include "debug/Override.hh"
#include "sim/system.hh"

namespace gem5
{
namespace o3
{

namespace
{

uint64_t
sv39NonLeafPte(Addr next_level_table_pa)
{
    RiscvISA::PTE pte = 0;
    pte.v = 1;
    pte.ppn = next_level_table_pa >> RiscvISA::PGSHFT;
    return static_cast<uint64_t>(pte);
}

uint64_t
sv39LeafPte(Addr pa, bool readable, bool writable, bool executable)
{
    RiscvISA::PTE pte = 0;
    pte.v = 1;
    pte.r = readable ? 1 : 0;
    pte.w = writable ? 1 : 0;
    pte.x = executable ? 1 : 0;
    pte.a = 1;
    pte.d = 1;
    pte.ppn = pa >> RiscvISA::PGSHFT;
    return static_cast<uint64_t>(pte);
}

} // anonymous namespace

TraceFetch::TraceFetch(Fetch &fetch_, const BaseO3CPUParams &params)
    : fetch(fetch_)
{
    for (int i = 0; i < MaxThreads; i++) {
        traceFetchExpectedCorrectIdx[i] = 1;
    }

    traceMode = params.enableTraceMode;
    traceFormat = params.traceFormat;
    traceTimingPTW = params.traceTimingPTW;
    tracePTReservedBytes = params.tracePTReservedBytes;
    tracePTLeafPageSize = params.tracePTLeafPageSize;
    traceAddrBase = params.traceAddrBase;
    traceAddrSize = params.traceAddrSize;
    if (traceMode) {
        DPRINTF(Fetch, "Trace mode enabled, file: %s, format: %s\n",
                params.traceFile, params.traceFormat);
        traceTrainBranches = params.traceTrainBranches;
        traceDecoupledFrontend = params.enableDecoupledBPInTrace;
        traceCheckpointInterval = params.traceCheckpointInterval;
        // Wire CPU params to fetch trace modeling knobs
        traceMispredictPenalty = params.traceMispredictPenalty;
        traceEnableWrongPath = params.traceEnableWrongPath;
        // Wrong-path injection mode (default NOPs)
        traceWrongPathUseTraceInst = params.traceWrongPathUseTraceInst;
        fatal_if(traceWrongPathUseTraceInst,
                 "traceWrongPathUseTraceInst is currently unimplemented");
        // Enable BP-vs-trace validation independent of training
        traceBPValidation = params.traceBPValidation;
        // Note: pass CPU as parent stats group; keep reader name simple to avoid
        // duplicated prefixes in stats paths.
        traceReader = createTraceReader(params.traceFormat, params.traceFile,
                                        "traceReader",
                                        params.traceAddrBase, params.traceAddrSize,
                                        params.traceAddrMapMode, params.traceAddrPageAlign,
                                        fetch.cpu);
        if (!traceReader) {
            fatal("Failed to create trace reader for format: %s\n",
                  params.traceFormat);
        }
    } else {
        traceReader = nullptr;
    }
}

TraceFetch::~TraceFetch() = default;

bool
TraceFetch::initializeTraceReader()
{
    if (!traceReader) {
        return false;
    }

    DPRINTF(Fetch, "Initializing trace reader\n");
    bool success = traceReader->init();
    if (!success) {
        warn("Failed to initialize trace reader\n");
        return false;
    }

    DPRINTF(Fetch, "Trace reader initialized successfully\n");
    // 同步清理 reader 内部缓冲/历史窗口，保持状态一致
    traceReader->resetHistory();
    return true;
}

void
TraceFetch::setupTraceTimingPTW(::gem5::ThreadContext *tc)
{
    if (!traceMode || !traceTimingPTW) {
        return;
    }

    if (!tc) {
        fatal("Trace timing PTW enabled but ThreadContext is null\n");
    }

    if (tracePTReservedBytes == 0) {
        fatal("Trace timing PTW enabled but tracePTReservedBytes is 0\n");
    }

    if (tracePTLeafPageSize != 4 * 1024 && tracePTLeafPageSize != 2 * 1024 * 1024) {
        fatal("Trace timing PTW: unsupported tracePTLeafPageSize=%llu\n",
              (unsigned long long)tracePTLeafPageSize);
    }

    constexpr Addr kPageSize = 4 * 1024;
    constexpr size_t kEntriesPerPt = 512;
    constexpr Addr kL0Coverage = static_cast<Addr>(kEntriesPerPt) * kPageSize;   // 2MiB
    constexpr Addr kL1Coverage = static_cast<Addr>(kEntriesPerPt) * kL0Coverage; // 1GiB

    Addr va_start = roundDown(traceAddrBase, kPageSize);
    Addr va_end = roundUp(traceAddrBase + traceAddrSize, kPageSize);
    if (va_end <= va_start) {
        fatal("Trace timing PTW: invalid trace mapping window base=0x%llx size=0x%llx\n",
              (unsigned long long)traceAddrBase, (unsigned long long)traceAddrSize);
    }

    Addr pt_region_base = traceAddrBase + traceAddrSize;
    if (pt_region_base % kPageSize) {
        fatal("Trace timing PTW: page table region base is not 4KiB-aligned "
              "(base=0x%llx)\n",
              (unsigned long long)pt_region_base);
    }
    Addr pt_region_end = pt_region_base + tracePTReservedBytes;
    fatal_if(pt_region_end <= pt_region_base,
             "Trace timing PTW: invalid page table region "
             "(base=0x%llx, end=0x%llx)\n",
             (unsigned long long)pt_region_base,
             (unsigned long long)pt_region_end);

    if (pt_region_base < va_end) {
        fatal("Trace timing PTW: page table region overlaps trace mapping window "
              "(trace=[0x%llx,0x%llx), pt=[0x%llx,0x%llx))\n",
              (unsigned long long)va_start, (unsigned long long)va_end,
              (unsigned long long)pt_region_base, (unsigned long long)pt_region_end);
    }

    auto *system = tc->getSystemPtr();
    if (!system) {
        fatal("Trace timing PTW enabled but System is null\n");
    }
    fatal_if(!system->isMemAddr(pt_region_base) ||
             !system->isMemAddr(pt_region_end - 1),
             "Trace timing PTW: page table region is outside physical memory "
             "(pt=[0x%llx,0x%llx))\n",
             (unsigned long long)pt_region_base,
             (unsigned long long)pt_region_end);

    PortProxy &phys = system->physProxy;
    phys.memsetBlob(pt_region_base, 0, tracePTReservedBytes);

    const uint64_t first_vpn2 = va_start / kL1Coverage;
    const uint64_t last_vpn2 = (va_end - 1) / kL1Coverage;
    const uint64_t num_l1_pages = last_vpn2 - first_vpn2 + 1;

    uint64_t num_leaf_pages = 0;
    if (tracePTLeafPageSize == kPageSize) {
        const uint64_t first_vpn1 = va_start / kL0Coverage;
        const uint64_t last_vpn1 = (va_end - 1) / kL0Coverage;
        num_leaf_pages = last_vpn1 - first_vpn1 + 1;
    }

    const uint64_t total_pt_pages =
        1 + num_l1_pages + (tracePTLeafPageSize == kPageSize ? num_leaf_pages : 0);
    const uint64_t required_bytes = total_pt_pages * kPageSize;
    if (required_bytes > tracePTReservedBytes) {
        fatal("Trace timing PTW: reserved region too small "
              "(required=0x%llx, reserved=0x%llx)\n",
              (unsigned long long)required_bytes,
              (unsigned long long)tracePTReservedBytes);
    }

    Addr root_pa = pt_region_base;
    Addr next_free = root_pa + kPageSize;

    std::array<uint64_t, kEntriesPerPt> root_entries{};
    root_entries.fill(0);

    for (uint64_t vpn2 = first_vpn2; vpn2 <= last_vpn2; ++vpn2) {
        Addr l1_pa = next_free;
        next_free += kPageSize;

        const uint64_t vpn2_idx = vpn2 & (kEntriesPerPt - 1);
        root_entries[vpn2_idx] = sv39NonLeafPte(l1_pa);

        std::array<uint64_t, kEntriesPerPt> l1_entries{};
        l1_entries.fill(0);

        Addr v2_base = vpn2 * kL1Coverage;
        Addr v2_start = std::max(va_start, v2_base);
        Addr v2_end = std::min(va_end, v2_base + kL1Coverage);
        if (v2_start >= v2_end) {
            continue;
        }

        const uint64_t first_vpn1 = v2_start / kL0Coverage;
        const uint64_t last_vpn1 = (v2_end - 1) / kL0Coverage;

        for (uint64_t vpn1 = first_vpn1; vpn1 <= last_vpn1; ++vpn1) {
            Addr v1_base = vpn1 * kL0Coverage;
            Addr v1_start = std::max(v2_start, v1_base);
            Addr v1_end = std::min(v2_end, v1_base + kL0Coverage);

            const uint64_t vpn1_idx = vpn1 & (kEntriesPerPt - 1);

            if (tracePTLeafPageSize == kL0Coverage) {
                l1_entries[vpn1_idx] = sv39LeafPte(v1_base, true, true, true);
                continue;
            }

            Addr l0_pa = next_free;
            next_free += kPageSize;
            l1_entries[vpn1_idx] = sv39NonLeafPte(l0_pa);

            std::array<uint64_t, kEntriesPerPt> l0_entries{};
            l0_entries.fill(0);

            const uint64_t first_vpn0 = v1_start / kPageSize;
            const uint64_t last_vpn0 = (v1_end - 1) / kPageSize;
            for (uint64_t vpn0 = first_vpn0; vpn0 <= last_vpn0; ++vpn0) {
                const uint64_t vpn0_idx = vpn0 & (kEntriesPerPt - 1);
                Addr page_base = vpn0 * kPageSize;
                l0_entries[vpn0_idx] = sv39LeafPte(page_base, true, true, true);
            }

            phys.writeBlob(l0_pa, l0_entries.data(), kPageSize);
        }

        phys.writeBlob(l1_pa, l1_entries.data(), kPageSize);
    }

    phys.writeBlob(root_pa, root_entries.data(), kPageSize);

    tc->setMiscReg(RiscvISA::MiscRegIndex::MISCREG_PRV,
                   static_cast<RegVal>(RiscvISA::PrivilegeMode::PRV_S));
    RiscvISA::SATP satp = 0;
    satp.mode = RiscvISA::AddrXlateMode::SV39;
    satp.asid = 0;
    satp.ppn = root_pa >> RiscvISA::PGSHFT;
    tc->setMiscReg(RiscvISA::MiscRegIndex::MISCREG_SATP,
                   static_cast<RegVal>(satp));

    DPRINTF(Fetch,
            "Trace timing PTW: installed SATP(root=0x%llx), PRV=S, "
            "trace=[0x%llx,0x%llx), pt=[0x%llx,0x%llx), leaf=%llu\n",
            (unsigned long long)root_pa,
            (unsigned long long)va_start, (unsigned long long)va_end,
            (unsigned long long)pt_region_base, (unsigned long long)pt_region_end,
            (unsigned long long)tracePTLeafPageSize);
}

bool
TraceFetch::initTraceMode()
{
    if (!initializeTraceReader()) {
        return false;
    }

    if (traceReader->isEOF()) {
        return true;
    }

    o3::TraceInstruction firstInstr = traceReader->getNextInstruction();
    if (!firstInstr.isValid()) {
        return false;
    }

    traceReader->reset();
    if (!initializeTraceReader()) {
        return false;
    }

    std::unique_ptr<PCStateBase> tracePC(fetch.threads[0].fetchpc->clone());
    auto& riscv_pc = tracePC->as<RiscvISA::PCState>();
    riscv_pc.set(firstInstr.getPC());
    set(fetch.threads[0].fetchpc, *tracePC);
    fetch.cpu->pcState(*tracePC, 0);

    auto* tc0 = fetch.cpu->getContext(0);
    if (tc0) {
        tc0->pcState(*tracePC);
        RegVal status = tc0->readMiscReg(RiscvISA::MiscRegIndex::MISCREG_STATUS);
        status |= RiscvISA::STATUS_FS_MASK;
        tc0->setMiscReg(RiscvISA::MiscRegIndex::MISCREG_STATUS, status);
    }

    if (tc0) {
        setupTraceTimingPTW(tc0);
    } else if (traceTimingPTW) {
        fatal("Trace timing PTW enabled but ThreadContext[0] is null\n");
    }

    DPRINTF(Fetch,
            "Trace mode: Set initial PC to 0x%llx from first trace instruction\n",
            firstInstr.getPC());
    if (tc0) {
        DPRINTF(Fetch,
                "Trace mode: fetch PC = 0x%llx, cpu PC = 0x%llx, TC PC = 0x%llx\n",
                fetch.threads[0].fetchpc->instAddr(), fetch.cpu->pcState(0).instAddr(),
                tc0->pcState().instAddr());
    } else {
        DPRINTF(Fetch,
                "Trace mode: fetch PC = 0x%llx, cpu PC = 0x%llx (no TC)\n",
                fetch.threads[0].fetchpc->instAddr(), fetch.cpu->pcState(0).instAddr());
    }

    if (fetch.branchPred) {
        DPRINTF(Fetch, "Trace mode: Priming decoupled BPU with start PC 0x%llx\n",
                firstInstr.getPC());

        assert(fetch.dbpbtb);
        fetch.dbpbtb->resetPC(firstInstr.getPC());
    }

    return true;
}

void
TraceFetch::resetStage()
{
    for (ThreadID tid = 0; tid < fetch.numThreads; ++tid) {
        // 正确路径的期望 trace 索引从 1 开始
        traceFetchExpectedCorrectIdx[tid] = 1;
    }

    // Reset trace consumption counter for precise seqNum→trace index mapping
    // Start consumed trace index from 1 to make indices human-friendly and
    // consistent with reader semantics used elsewhere.
    traceInstrConsumed = 1;
}

bool
TraceFetch::maybeStallFetch(ThreadID tid)
{
    static_cast<void>(tid);
    if (!traceMode) {
        return false;
    }

    // If we're modeling a generic mispredict stall (coupled frontend), stall for this cycle
    if (traceStallRemaining > Cycles(0)) {
        traceStallRemaining = traceStallRemaining - Cycles(1);
        auto stall_left = (unsigned long long) traceStallRemaining;
        DPRINTF(Fetch, "[tid:%i] Trace mispredict stall active, remaining=%llu cycles\n",
                tid, stall_left);
        return true;
    }

    return false;
}

void
TraceFetch::ensureTraceStreamFilled(ThreadID tid, size_t min_count)
{
    if (!traceMode || !traceReader) {
        return;
    }
    if (traceEnableWrongPath && traceWrongPathActive) {
        return;
    }
    while (traceExpectedStream[tid].size() < min_count) {
        auto ti = traceReader->getNextInstruction();
        if (!ti.isValid()) {
            break;
        }
        DPRINTF(Fetch, "[TraceStream] Fetched PC=0x%lx (sn:%llu)\n",
                ti.getPC(), (unsigned long long)ti.getSeqNum());
        traceExpectedStream[tid].push_back(ti);
    }
}

StallReason
TraceFetch::checkMemoryNeeds(ThreadID tid, const PCStateBase &this_pc)
{
    // 防御：正常情况下 traceMode 必然伴随有效的 traceReader
    panic_if(!traceReader, "traceMode enabled but traceReader is unavailable");
    return fetchTraceInstruction(tid, this_pc);
}

StallReason
TraceFetch::fetchTraceInstruction(ThreadID tid, const PCStateBase &this_pc)
{
    const bool wrong_path = (traceEnableWrongPath && traceWrongPathActive);
    if (wrong_path) {
        const unsigned nop_size =
            chooseWrongPathNopSize(tid, this_pc.instAddr());
        // RISC-V 32b nop: 0x00000013; 16b compressed nop: 0x0001
        TheISA::MachInst nop = (nop_size == 2)
            ? static_cast<TheISA::MachInst>(0x0001u)
            : static_cast<TheISA::MachInst>(0x00000013u);
        supplyTraceToDecoder(
            tid, this_pc, nop, this_pc.instAddr(),
            nop_size == 2 ? "supplied 2B NOP without advancing reader"
                          : "supplied 4B NOP without advancing reader (pred takenPC)");
        return StallReason::NoStall;
    }

    // 正确路径：填充期望指令流并供码（不在此处消费流，仅在构建后比对并消耗）
    ensureTraceStreamFilled(tid, TRACE_STREAM_MIN_FILL);
    if (traceExpectedStream[tid].empty()) {
        DPRINTF(Fetch, "[tid:%i] Trace on-demand: expected stream empty (EOF=%d)\n",
                tid, traceReader->isEOF());
        return StallReason::IcacheStall;
    }
    auto head = traceExpectedStream[tid].front();
    // 对非分支/异常类 ctrl-flow-change，在 decoupled + wrong-path 校验场景下，
    // 若缺乏可靠 nextPC，保守将长度标为 2B，避免后续进入 wrong-path 时跨过块内预测点。
    if (traceEnableWrongPath && traceBPValidation &&
        head.isCtrlFlowChange() && !head.isAnyBranch()) {
        head.setInstSizeBytes(2);
    }
    pendingTraceInstr = head;
    pendingTraceValid = true;
    TheISA::MachInst machInst = createMachInstFromTrace(head);
    supplyTraceToDecoder(tid, this_pc, machInst, head.getPC(),
                         "supplied 4B to decoder (from expected stream head)");
    return StallReason::NoStall;
}

void
TraceFetch::supplyTraceToDecoder(ThreadID tid, const PCStateBase &this_pc,
                                 TheISA::MachInst machInst, Addr instrPC,
                                 const char *tag)
{
    auto *dec_ptr = fetch.decoder[tid];
    memcpy(dec_ptr->moreBytesPtr(), &machInst, sizeof(machInst));
    fetch.decoder[tid]->moreBytes(this_pc, instrPC);
    fetch.threads[tid].startPC = instrPC;
    fetch.threads[tid].valid = true;
    DPRINTF(Fetch, "[tid:%i] Trace on-demand: %s at PC=0x%llx\n",
            tid, tag, (unsigned long long)instrPC);
}

void
TraceFetch::enterTraceWrongPath(ThreadID tid, InstSeqNum branchSeqNum, Addr predPC,
                                Addr corrPC, bool forceMinStep,
                                const char *reason, uint64_t traceSeqNum)
{
    traceWrongPathActive       = true;
    traceWrongPathBranchSeqNum = branchSeqNum;
    traceWrongPathForceMinStep = forceMinStep;
    traceWrongPathPredPC       = predPC;
    traceWrongPathCorrectPC    = corrPC;
    DPRINTF(Fetch,
            "[tid:%i] %s (predPC=0x%llx, corrPC=0x%llx, sn:%llu, tracesn:%llu)\n",
            tid, reason,
            (unsigned long long)predPC,
            (unsigned long long)corrPC,
            (unsigned long long)traceWrongPathBranchSeqNum,
            (unsigned long long)traceSeqNum);
}

void
TraceFetch::exitTraceWrongPath(ThreadID tid, const char *reason)
{
    DPRINTF(Fetch, "[tid:%i] Exit wrong-path mode: %s\n", tid, reason);
    traceWrongPathActive = false;
    traceWrongPathForceMinStep = false;
    traceWrongPathPredPC = 0;
    traceWrongPathCorrectPC = 0;
    traceWrongPathBranchSeqNum = 0;
}

unsigned
TraceFetch::chooseWrongPathNopSize(ThreadID tid, Addr pc)
{
    static_cast<void>(tid);
    if (traceWrongPathForceMinStep) {
        return 2;
    }
    unsigned sz = 2;
    Addr block_end = 0;
    Addr taken_pc = 0;
    bool taken = false;
    if (fetch.isBTBPred()) {
        assert(fetch.dbpbtb);
        if (fetch.dbpbtb->ftqHasFetching(tid)) {
            const auto prediction = fetch.dbpbtb->ftqFetchBlock(tid);
            block_end = prediction.endPC;
            taken_pc = prediction.controlPC;
            taken = prediction.taken;
        }
    }
    // If the current PC matches predicted takenPC (assume 4B branches), use a 4B NOP.
    if (taken && taken_pc && pc == taken_pc) {
        sz = 4;
    } else if (block_end && pc + 2 == block_end) {
        sz = 2;
    }
    return sz;
}

void
TraceFetch::bindPendingTraceMetadata(ThreadID tid, const DynInstPtr &instruction,
                                     const PCStateBase &pc,
                                     o3::TraceInstruction &traceForThisInst)
{
    traceForThisInst = o3::TraceInstruction();

    // 在按需 trace 模式下，将挂起的 trace 元数据绑定到 DynInst
    // 保留一份本条指令的 trace 记录用于后续 BP 校验
    if (instruction && traceMode && pendingTraceValid) {
        bindTraceMetadata(instruction, pendingTraceInstr, tid);
        traceForThisInst = pendingTraceInstr;
    }

    // Fetch-side严格顺序校验（流式）：正确路径构建指令后，与期望流head对比并消耗
    if (instruction && traceMode && traceEnableWrongPath && !traceWrongPathActive) {
        validateAndConsumeTraceStream(tid, pc);
    }
}

void
TraceFetch::postBranchPredict(ThreadID tid, const DynInstPtr &instruction,
                              const o3::TraceInstruction &traceForThisInst,
                              PCStateBase &pc, PCStateBase &next_pc,
                              bool predictedBranch)
{
    // 非分支或 cond-trap 控制流改变（例如异常/陷入）在 decoupled + wrong-path 模式下视为
    // "trace 驱动的 trap wrong-path"：从该指令之后沿 BPU 预测路径走的指令
    // 对 traceReader 而言都是 wrong-path，后续由 commit 触发 trap squash 统一纠正。
    if (traceMode && instruction && traceForThisInst.isValid() &&
        traceForThisInst.isCtrlFlowChange() &&
        traceEnableWrongPath) {
        maybeEnterTraceCtrlFlowWrongPath(tid, instruction, traceForThisInst, pc, next_pc);
    }

    // 若启用 BP 校验，则将 BPU 预测与 trace 真值对比，决定是否进入 wrong-path。
    // 注意：带异常/陷入（ctrlFlowChange）的指令由上方 trap wrong-path 逻辑统一处理，
    // 这里仅处理“正常控制流”的分支/非分支指令，避免重复、语义混淆。
    if (traceMode && traceBPValidation && instruction &&
        traceForThisInst.isValid() &&
        !traceForThisInst.isCtrlFlowChange()) {
        handleTraceBPValidation(tid, instruction, traceForThisInst, next_pc, predictedBranch);
    }

    // 清除 pending 标记
    if (traceMode && pendingTraceValid) {
        pendingTraceValid = false;
    }
}

void
TraceFetch::clearPending()
{
    pendingTraceValid = false;
}

void
TraceFetch::handleTraceSquash(ThreadID tid, const PCStateBase &new_pc,
                              const DynInstPtr squashInst, InstSeqNum seqNum)
{
    // Clean up trace instruction metadata for squashed instructions
    if (!traceMode) {
        return;
    }

    bool allow_rb = true;
    bool squash_itself = false;
    auto trace_rb_seqnum = seqNum;
    if (traceWrongPathActive) {
        // 处于 wrong-path：优先处理边界分支产生的 squash。
        // 注意：也可能出现非边界的 squash（例如 TLB/page fault、trap、重放），
        // 此时 squashInst 可能为空。对这类情况不应 panic，而是温和退出 wrong-path。
        DPRINTF(Fetch, "[tid:%i] In wrong-path, processing squash for trace rollback, wrong path seqnum is %llu\n",
            tid, (unsigned long long)traceWrongPathBranchSeqNum);
        if (squashInst) {
            DPRINTF(Fetch, "[tid:%i] In wrong-path, detected squash from inst (sn:%llu->tracesn:%llu)\n",
                    tid,
                    (unsigned long long)squashInst->seqNum,
                    findTraceIndexForSeqNum(squashInst->seqNum));
            if (squashInst->seqNum == traceWrongPathBranchSeqNum) {
                DPRINTF(Fetch,
                        "[tid:%i] In wrong-path, detected squash from "
                        "mispredicted inst (sn:%llu->tracesn:%llu), trigger trace rollback\n",
                        tid,
                        (unsigned long long)traceWrongPathBranchSeqNum,
                        findTraceIndexForSeqNum(traceWrongPathBranchSeqNum));
                // check whether new pc is correct
                bool is_correct_target = new_pc.instAddr() == traceWrongPathCorrectPC;
                if (is_correct_target) {
                    DPRINTF(Fetch,
                            "[tid:%i] Squash target PC (0x%#lx) matches correct PC (0x%#lx)\n",
                            tid, new_pc.instAddr(), traceWrongPathCorrectPC);
                    exitTraceWrongPath(tid, "mispred boundary squash reaches correct PC");
                } else {
                    DPRINTF(Fetch,
                            "[tid:%i] Warning: Squash target PC (0x%#lx) does not match "
                            "correct PC (0x%#lx)\n",
                            tid, new_pc.instAddr(), traceWrongPathCorrectPC);
                    // stay in wrong-path, let later squash handle
                }
            } else if (squashInst->seqNum < traceWrongPathBranchSeqNum) {
                DPRINTF(Fetch,
                        "[tid:%i] In wrong-path, detected squash from inst "
                        "(sn:%llu->tracesn:%llu) prior to mispredicted inst (sn:%llu)\n",
                        tid,
                        (unsigned long long)squashInst->seqNum,
                        findTraceIndexForSeqNum(squashInst->seqNum),
                        (unsigned long long)traceWrongPathBranchSeqNum);
                exitTraceWrongPath(tid, "squash before mispredicted branch");
            } else {
                allow_rb = false;
                DPRINTF(Fetch,
                        "[tid:%i] In wrong-path, skip trace rollback for "
                        "non-boundary squash (sn:%llu)\n",
                        tid, (unsigned long long)seqNum);
            }
            if (squashInst->getPC() == new_pc.instAddr()) {
                squash_itself = true;
                DPRINTF(Fetch, "Squashing inst squashing itself, probably load replay (pc: 0x%#lx)\n",
                    squashInst->getPC());
            }
        } else {
            DPRINTF(Fetch, "[tid:%i] In wrong-path, detected squash from non-inst event (sn:%llu->tracesn:%llu)\n",
                    tid,
                    (unsigned long long)seqNum,
                    findTraceIndexForSeqNum(seqNum));
            if (new_pc.instAddr() == traceWrongPathCorrectPC) {
                // non-inst squash (例如 trap squash) 把 PC 直接带回了正确路径
                // traceReader 在 wrong-path 期间未前进，因此此处无需回滚，只需退出
                // wrong-path 模式即可。
                squash_itself = false;
                trace_rb_seqnum = traceWrongPathBranchSeqNum;
                exitTraceWrongPath(tid, "non-inst squash reached correct PC");
                // allow_rb = false; // 不需要触碰 traceReader
                DPRINTF(Fetch,
                        "[tid:%i] In wrong-path, non-inst squash reached "
                        "correct PC (0x%#llx); exit wrong-path without rollback "
                        "(sn:%llu)\n",
                        tid,
                        (unsigned long long)new_pc.instAddr(),
                        (unsigned long long)seqNum);
            } else if (seqNum <= traceWrongPathBranchSeqNum) {
                DPRINTF(Fetch,
                        "[tid:%i] In wrong-path, detected squash from "
                        "non-inst event (sn:%llu->tracesn:%llu) prior to mispredicted inst (sn:%llu), "
                        "trigger trace rollback\n",
                        tid,
                        (unsigned long long)seqNum,
                        findTraceIndexForSeqNum(seqNum),
                        (unsigned long long)traceWrongPathBranchSeqNum);
                trace_rb_seqnum = seqNum + 1; // for non-inst squash before branch, rollback to seqNum + 1
                squash_itself = true;
                exitTraceWrongPath(tid, "non-inst squash before mispredicted branch");
                // this would happen for memory violation
            } else {
                allow_rb = false;
                DPRINTF(Fetch,
                        "[tid:%i] In wrong-path, skip trace rollback for "
                        "non-boundary squash (sn:%llu) without squashInst\n",
                        tid, (unsigned long long)seqNum);
            }
        }
    } else {
        // not in wrong-path: normal squash, rollback to squashInst seqNum
        if (squashInst) {
            trace_rb_seqnum = squashInst->seqNum;
            DPRINTF(Fetch, "[tid:%i] Normal squash to seqNum %llu from inst (sn:%llu->tracesn:%llu)\n",
                    tid,
                    (unsigned long long)trace_rb_seqnum,
                    (unsigned long long)squashInst->seqNum,
                    findTraceIndexForSeqNum(squashInst->seqNum));
            if (squashInst->getPC() == new_pc.instAddr()) {
                squash_itself = true;
                DPRINTF(Fetch, "Squashing inst squashing itself, probably load replay (pc: 0x%#lx)\n",
                    squashInst->getPC());
            }
        } else {
            // non-inst squash (e.g., TLB/page fault, trap, replay)
            trace_rb_seqnum = seqNum + 1;
            DPRINTF(Fetch, "[tid:%i] Normal squash to seqNum %llu from non-inst event (sn:%llu->tracesn:%llu)\n",
                    tid,
                    (unsigned long long)trace_rb_seqnum,
                    (unsigned long long)seqNum,
                    findTraceIndexForSeqNum(seqNum));
            squash_itself = true;
        }
        traceWrongPathForceMinStep = false;
    }

    if (allow_rb) {
        DPRINTF(Fetch, "[tid:%i] Rolling back trace reader to seqNum %llu, squash_itself=%d\n",
                tid, (unsigned long long)trace_rb_seqnum, squash_itself);
        cleanupTraceMetadata(trace_rb_seqnum);
        // Rollback trace reader to handle misprediction
        if (!rollbackTraceReader(trace_rb_seqnum, squash_itself)) {
            DPRINTF(Fetch, "[tid:%i] Warning: Failed to rollback trace reader to seqNum %llu\n",
                    tid, (unsigned long long)trace_rb_seqnum);
        }
        // 回滚后清空期望流，避免与reader位置不一致
        traceExpectedStream[tid].clear();
        DPRINTF(Fetch, "[tid:%i] Cleared expected trace stream after rollback\n", tid);
    }
}

void
TraceFetch::storeTraceInstMetadata(InstSeqNum seqNum, const o3::TraceInstruction &traceInstr)
{
    // Create shared_ptr to avoid large object copy that caused segfault
    auto traceInstrPtr = std::make_shared<const o3::TraceInstruction>(traceInstr);
    traceInstMap[seqNum] = traceInstrPtr;

    // Stats: count stored metadata records
    fetch.fetchStats.traceMetaStores++;

    DPRINTF(Fetch, "[sn:%lli] Stored trace instruction metadata (shared_ptr at %p)\n",
            seqNum, traceInstrPtr.get());
}

const o3::TraceInstruction*
TraceFetch::getTraceInstMetadata(InstSeqNum seqNum) const
{
    auto it = traceInstMap.find(seqNum);
    if (it != traceInstMap.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool
TraceFetch::isTraceInstruction(InstSeqNum seqNum) const
{
    return traceInstMap.find(seqNum) != traceInstMap.end();
}

void
TraceFetch::cleanupTraceMetadata(InstSeqNum seqNum)
{
    // Remove trace metadata for all instructions with seqNum >= threshold
    Counter removed = 0;
    auto it = traceInstMap.begin();
    while (it != traceInstMap.end()) {
        if (it->first > seqNum) {
            DPRINTF(Fetch, "[sn:%lli] Removing trace metadata due to squash\n", it->first);
            it = traceInstMap.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    // Also clean up sequence number to trace index mapping
    auto seqIt = seqNumToTraceIndex.begin();
    while (seqIt != seqNumToTraceIndex.end()) {
        if (seqIt->first > seqNum) {
            DPRINTF(Fetch, "[sn:%lli] Removing seqNum to trace index mapping due to squash\n", seqIt->first);
            seqIt = seqNumToTraceIndex.erase(seqIt);
            ++removed;
        } else {
            ++seqIt;
        }
    }

    // Stats: record cleanup calls and total removed entries
    fetch.fetchStats.traceMetaCleanupSquashCalls++;
    fetch.fetchStats.traceMetaCleanupSquashEntries += removed;
}

void
TraceFetch::cleanupTraceMetadataOnCommit(InstSeqNum /*seqNum*/)
{
    // Sliding-window cleanup: keep a guard window behind the oldest in-flight
    // instruction and (if active) behind the wrong-path boundary. This avoids
    // removing metadata that may still be needed by a late-arriving squash.

    static constexpr uint64_t TRACE_META_GUARD = 256; // conservative default

    const InstSeqNum oldest_inflight = fetch.cpu->getOldestInFlightSeqNum();
    const InstSeqNum wp_boundary = traceWrongPathActive ? traceWrongPathBranchSeqNum
                                                        : std::numeric_limits<InstSeqNum>::max();
    const InstSeqNum keep_min = std::min(oldest_inflight, wp_boundary);
    const InstSeqNum safe_threshold = (keep_min > TRACE_META_GUARD)
                                          ? (keep_min - TRACE_META_GUARD)
                                          : 0;

    Counter removed = 0;

    // Erase entries with seqNum strictly less than safe_threshold
    for (auto it = traceInstMap.begin(); it != traceInstMap.end(); ) {
        if (it->first < safe_threshold) {
            it = traceInstMap.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    for (auto it = seqNumToTraceIndex.begin(); it != seqNumToTraceIndex.end(); ) {
        if (it->first < safe_threshold) {
            it = seqNumToTraceIndex.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    DPRINTF(Fetch,
            "[TraceMetaCleanup] oldest_inflight=%llu wp_active=%d wp_boundary=%llu "
            "guard=%llu threshold=%llu removed=%llu\n",
            (unsigned long long)oldest_inflight,
            (int)traceWrongPathActive,
            (unsigned long long)wp_boundary,
            (unsigned long long)TRACE_META_GUARD,
            (unsigned long long)safe_threshold,
            (unsigned long long)removed);

    // Stats: count commit-side cleanups
    fetch.fetchStats.traceMetaCleanupCommitCalls++;
}

void
TraceFetch::maybeCreateTraceCheckpoint(InstSeqNum seqNum)
{
    if (!traceMode || !traceReader) {
        return;
    }

    // Create checkpoint every traceCheckpointInterval instructions.
    if (traceCheckpointInterval != 0 && (seqNum % traceCheckpointInterval) == 0) {
        auto checkpoint = traceReader->createCheckpoint();
        if (checkpoint.valid) {
            traceCheckpoints.push_back(checkpoint);
            checkpointSeqNums.push_back(seqNum);
            DPRINTF(Fetch, "[sn:%lli] Created trace checkpoint at trace index %lu\n",
                    seqNum, checkpoint.instructionIndex);

            // Limit number of checkpoints to avoid memory growth
            const size_t MAX_CHECKPOINTS = 16;
            if (traceCheckpoints.size() > MAX_CHECKPOINTS) {
                traceCheckpoints.erase(traceCheckpoints.begin());
                checkpointSeqNums.erase(checkpointSeqNums.begin());
                DPRINTF(Fetch, "Removed oldest trace checkpoint\n");
            }
        }
    }
}

uint64_t
TraceFetch::findTraceIndexForSeqNum(InstSeqNum seqNum) const
{
    // First try direct lookup
    auto it = seqNumToTraceIndex.find(seqNum);
    if (it != seqNumToTraceIndex.end()) {
        DPRINTF(Fetch, "findTraceIndexForSeqNum[sn:%lli]: Direct mapping found: traceIndex=%lu\n",
                seqNum, it->second);
        return it->second;
    }

    // dump seqNumToTraceIndex
    DPRINTF(Fetch, seqNumToTraceIndex.size() == 0 ?
            "  seqNumToTraceIndex is empty\n" :
            "  seqNumToTraceIndex contents:\n");
    for (const auto& pair : seqNumToTraceIndex) {
        DPRINTF(Fetch, "  seqNum=%lli => traceIndex=%lu\n", pair.first, pair.second);
    }

    DPRINTF(Fetch, "findTraceIndexForSeqNum[sn:%lli]: No direct mapping\n", seqNum);
    return 0;
}

bool
TraceFetch::lookupTraceIndexForSeqNum(InstSeqNum seqNum, uint64_t &index) const
{
    auto it = seqNumToTraceIndex.find(seqNum);
    if (it != seqNumToTraceIndex.end()) {
        index = it->second;
        return true;
    }
    index = 0;
    return false;
}

bool
TraceFetch::rollbackTraceReader(InstSeqNum seqNum, bool squash_itself)
{
    if (!traceMode || !traceReader) {
        DPRINTF(Fetch, "rollbackTraceReader[sn:%lli]: Not in trace mode\n", seqNum);
        return false;
    }

    bool need_to_decrement_index = squash_itself;
    // Find trace index to rollback to (1-based). We want the next getNextInstruction()
    // to return the instruction at 'index'.
    uint64_t index = findTraceIndexForSeqNum(seqNum);
    bool found = index != 0;
    if (!found) {
        if (squash_itself) {
            // If squashing the instruction itself, try one earlier
            DPRINTF(Fetch,
                    "rollbackTraceReader[sn:%lli]: No mapped trace index, "
                    "trying earlier instruction\n",
                    seqNum);
            index = findTraceIndexForSeqNum(seqNum - 1);
            if (index != 0) {
                found = true;
                need_to_decrement_index = false; // already moved back
            } else {
                DPRINTF(Fetch, "rollbackTraceReader[sn:%lli]: No mapped trace index (skip)\n", seqNum);
                return false;
            }
        }
    }

    if (need_to_decrement_index) {
        // If squashing the instruction itself, we need to go back one more instruction
        if (index > 0) {
            DPRINTF(Fetch, "rollbackTraceReader[sn:%lli]: Squashing itself, moving back one instruction\n", seqNum);
            --index;
        } else {
            DPRINTF(Fetch, "rollbackTraceReader[sn:%lli]: Cannot move back before start of trace\n", seqNum);
            return false;
        }
    }

    // 交由 TraceReader 软回滚（命中本地历史窗口则不触碰文件指针），超界时内部自行降级
    const uint64_t seek_cursor = (index > 0) ? (index - 1) : 0;
    const bool success = traceReader->softSeekToInstruction(seek_cursor);
    DPRINTF(Fetch, "rollbackTraceReader[sn:%lli]: softSeekToInstruction(index=%lu,cursor=%lu) => %d\n",
            seqNum, index, seek_cursor, (int)success);
    return success;
}

bool
TraceFetch::validateBPPrediction(const o3::TraceInstruction& traceInstr,
                                 Addr predictedPC, bool predictedTaken)
{
    if (!traceInstr.getBranch()) {
        // Non-branch instructions always "match" since there's no prediction to validate
        return true;
    }

    bool traceWrong = (!traceInstr.getBranchTaken() && predictedTaken) || (
        traceInstr.getBranchTaken() && traceInstr.getBranchTarget() != predictedPC);
    DPRINTF(Fetch,
            "validateBPPrediction: PC=0x%lx, predictedPC=0x%lx, actualTarget=0x%lx, "
            "predictedTaken=%d, actualTaken=%d\n",
            traceInstr.getPC(), predictedPC,
            traceInstr.getBranchTarget(),
            predictedTaken, traceInstr.getBranchTaken());

    if (traceInstr.getBranchTaken()) {
        DPRINTF(Fetch, "validateBPPrediction: Branch taken, predicted=%d, actual=%d\n",
                predictedTaken, traceInstr.getBranchTaken());
    } else {
        DPRINTF(Fetch, "validateBPPrediction: Branch not taken, predicted=%d, actual=%d\n",
                predictedTaken, traceInstr.getBranchTaken());
    }

    if (traceWrong) {
        DPRINTF(Fetch, "validateBPPrediction: MISPREDICTION detected - predicted=%d, actual=%d\n",
                predictedTaken, traceInstr.getBranchTaken());
    }

    return !traceWrong;
}

Addr
TraceFetch::getTracePCByIndex(uint64_t index)
{
    if (!traceMode || !traceReader) {
        return 0;
    }
    // Use checkpoint/restore to avoid observable side effects.
    auto ckpt = traceReader->createCheckpoint();
    Addr pc_val = 0;
    // 1-based indexing: we want the PC at instruction 'index'.
    // Our trace reader yields the next instruction after the current index,
    // so seek to (index-1) then read one instruction.
    if (index == 0) {
        // Undefined request under 1-based indexing; keep behavior safe.
        traceReader->restoreCheckpoint(ckpt);
        return 0;
    }
    bool ok = traceReader->seekToInstruction(index - 1);
    if (ok) {
        auto ti = traceReader->getNextInstruction();
        if (ti.isValid()) {
            pc_val = ti.getPC();
        }
    }
    // Restore regardless of success.
    traceReader->restoreCheckpoint(ckpt);
    return pc_val;
}

void
TraceFetch::bindTraceMetadata(const DynInstPtr &instruction,
                              const o3::TraceInstruction &traceInstr,
                              ThreadID tid)
{
    // 记录元数据映射（seqNum -> trace 指令）
    storeTraceInstMetadata(instruction->seqNum, traceInstr);
    seqNumToTraceIndex[instruction->seqNum] = traceInstr.getSeqNum() + 1;
    DPRINTF(Fetch,
            "[tid:%i] Bind trace metadata to [sn:%lli]->[tracesn:%lli]: "
            "pc=0x%llx, taken=%d, traceInstrConsumed=%llu\n",
            tid, instruction->seqNum,
            (unsigned long long)traceInstr.getSeqNum()+1,
            (unsigned long long)traceInstr.getPC(),
            traceInstr.getBranchTaken(),
            (unsigned long long)traceInstrConsumed);
    traceInstrConsumed++;

    // 为后端提供 trace 侧的 trap/异常控制流信息，用于跳过 decode 阶段的
    // 分支目标校验，由 trap/wrong-path 逻辑统一处理。
    instruction->setTraceCtrlFlowChange(traceInstr.isCtrlFlowChange());

    // 为 EXE 阶段提供分支真值以按普通路径重定向
    if (traceInstr.isAnyBranch()) {
        const bool taken = traceInstr.getBranchTaken();
        const bool hasTarget = traceInstr.getHasBranchTarget();
        const Addr target = hasTarget ? traceInstr.getBranchTarget() : 0;
        const Addr fallthrough = traceInstr.getFallThroughPC();
        instruction->setTraceBranchInfo(taken, hasTarget, target, fallthrough);
        // Trace hints for branch classification (used to override static decode in trace mode)
        instruction->setTraceIsCall(
            traceInstr.getInstType() == o3::TraceInstruction::InstType::CALL_DIRECT ||
            traceInstr.getInstType() == o3::TraceInstruction::InstType::CALL_INDIRECT);
        instruction->setTraceIsReturn(
            traceInstr.getInstType() == o3::TraceInstruction::InstType::RETURN);
        instruction->setTraceIsIndirect(
            traceInstr.getInstType() == o3::TraceInstruction::InstType::UNCOND_INDIRECT_BRANCH ||
            traceInstr.getInstType() == o3::TraceInstruction::InstType::CALL_INDIRECT ||
            traceInstr.getInstType() == o3::TraceInstruction::InstType::RETURN);
        DPRINTF(Fetch,
                "[tid:%i] Bind trace-branch info to [sn:%lli]: taken=%d, hasTgt=%d\n",
                tid, instruction->seqNum, taken, hasTarget);
        DPRINTF(Fetch,
                "[tid:%i] trace tgt=0x%llx, ft=0x%llx\n",
                tid, (unsigned long long)target,
                (unsigned long long)fallthrough);
    }

    // 如果该 trace 指令被标记为整个 trace 流中的最后一条，则将对应的
    // DynInst 也打上“最后一条 trace 指令”的标记，便于 Commit 阶段在其
    // 提交时执行自然退出逻辑。
    if (traceInstr.isLastInTrace()) {
        instruction->setLastTraceInst(true);
        DPRINTF(Fetch,
                "[tid:%i] Mark [sn:%lli] as LAST trace-driven instruction\n",
                tid, instruction->seqNum);
    }
}

void
TraceFetch::validateAndConsumeTraceStream(ThreadID tid, const PCStateBase &pc)
{
    if (traceExpectedStream[tid].empty()) {
        panic("[Fetch][tid:%d] Trace expected stream unexpectedly empty on correct path", tid);
    }

    const auto &head = traceExpectedStream[tid].front();
    Addr built_pc = pc.instAddr();
    DPRINTF(Fetch, "[TraceStream] Built PC=0x%lx vs Expected PC=0x%lx (sn:%llu)\n",
            (unsigned long)built_pc, (unsigned long)head.getPC(),
            (unsigned long long)head.getSeqNum());
    if (built_pc != head.getPC()) {
        panic("[Fetch][tid:%d] Trace stream PC mismatch: built=0x%lx expect=0x%lx (sn:%llu)",
              tid, (unsigned long)built_pc, (unsigned long)head.getPC(),
              (unsigned long long)head.getSeqNum());
    }

    // 可选：加上 MachInst 内容对比（同 createMachInstFromTrace(head)）
    traceExpectedStream[tid].pop_front();
}

void
TraceFetch::maybeEnterTraceCtrlFlowWrongPath(ThreadID tid,
                                             const DynInstPtr &instruction,
                                             const o3::TraceInstruction &traceInstr,
                                             PCStateBase &pc,
                                             PCStateBase &next_pc)
{
    const bool nonBranchTrap = !traceInstr.isAnyBranch();
    const bool branchTrap = traceInstr.isAnyBranch();

    if (nonBranchTrap || branchTrap) {
        Addr predicted_pc = next_pc.instAddr();
        const Addr corr_pc = traceInstr.getCtrlFlowTarget();

        if (!traceWrongPathActive) {
            // 非分支或未 taken/异常式 ctrlFlowChange：trace 无可靠 next_pc，保守采用 2B 步进。
            // 将预测 PC 折算成“pc+2”以免跨过块内的预测点。
            bool force_min_step = false;
            if (nonBranchTrap || !traceInstr.getBranchTaken()) {
                predicted_pc = instruction->pcState().instAddr() + 2;
                force_min_step = true; // 没有可靠 next_pc，用最小步长
            }
            enterTraceWrongPath(
                tid, instruction->seqNum, predicted_pc, corr_pc,
                force_min_step,
                nonBranchTrap ? "Enter non-branch-trap wrong-path; skip local BP squash/train"
                              : "Enter branch-trap wrong-path; skip local BP squash/train",
                traceInstr.getSeqNum());
        } else {
            DPRINTF(Fetch,
                    "[tid:%i] Already in trap-wrong-path mode; continue "
                    "without changes (predPC=0x%llx)\n",
                    tid, (unsigned long long)predicted_pc);
        }
    }
}

void
TraceFetch::handleTraceBPValidation(ThreadID tid, const DynInstPtr &instruction,
                                    const o3::TraceInstruction &traceInstr,
                                    const PCStateBase &next_pc,
                                    bool predictedBranch)
{
    const Addr predictedPC = next_pc.instAddr();
    const Addr ft_pc = traceInstr.getFallThroughPC();
    const bool predictedTaken = (predictedPC != ft_pc);

    if (traceInstr.isAnyBranch()) {
        const bool ok = validateBPPrediction(traceInstr, predictedPC, predictedTaken);

        if (!ok) {
            if (traceEnableWrongPath) {
                if (!traceWrongPathActive) {
                    Addr corr_target = traceInstr.getBranchTaken() && traceInstr.getHasBranchTarget()
                                        ? traceInstr.getBranchTarget()
                                        : ft_pc;
                    // decoupled/wrong-path：仅进入 wrong-path 模式，不在此处自发 squash/训练
                    enterTraceWrongPath(
                        tid, instruction->seqNum, predictedPC, corr_target,
                        false,
                        "Enter wrong-path mode; skip local BP squash/train",
                        traceInstr.getSeqNum());
                } else {
                    DPRINTF(Fetch,
                            "[tid:%i] Already in wrong-path mode; continue "
                            "without changes (predPC=0x%llx)\n",
                            tid, (unsigned long long)predictedPC);
                }
            } else {
                // coupled：维持原 stall 模型与本地 BP 校正
                traceStallRemaining = traceMispredictPenalty;
                DPRINTF(Override, "[TRACE-FTB] Stall model on mispredict: %llu cycles remaining\n",
                        (unsigned long long)traceStallRemaining);

                // 更正 BPU 历史（ground truth）
                Addr corr_target = traceInstr.getBranchTaken() && traceInstr.getHasBranchTarget()
                                    ? traceInstr.getBranchTarget()
                                    : ft_pc;
                DPRINTF(Fetch,
                        "[tid:%i] Trace/BP mismatch observed (predPC=0x%llx, corr=0x%llx); "
                        "defer correction to later stage squash\n",
                        tid, (unsigned long long)predictedPC,
                        (unsigned long long)corr_target);
            }
        }
    } else if (traceEnableWrongPath && predictedBranch && predictedTaken) {
        // BP 可能将非 branch 指令错误预测为 taken，
        // 此时也应进入 wrong-path，由后端 decode squash 纠正。
        if (!traceWrongPathActive) {
            enterTraceWrongPath(
                tid, instruction->seqNum, predictedPC, ft_pc,
                false,
                "Enter non-branch-predicted wrong-path; skip local BP squash/train",
                traceInstr.getSeqNum());
        } else {
            DPRINTF(Fetch,
                    "[tid:%i] Already in wrong-path mode; continue "
                    "after non-branch taken prediction (predPC=0x%llx)\n",
                    tid, (unsigned long long)predictedPC);
        }
    }
}

TheISA::MachInst
TraceFetch::createMachInstFromTrace(const o3::TraceInstruction &traceInstr)
{
    // Extract register information from trace
    const auto& srcRegs = traceInstr.getSrcRegs();
    const auto& dstRegs = traceInstr.getDstRegs();
    const uint8_t instSize = traceInstr.getInstSizeBytes() ? traceInstr.getInstSizeBytes() : 4;

    if (instSize != 2 && instSize != 4) {
        panic("[TRACE-ENC] Unsupported instruction size %u at PC=0x%llx (type=%d, sn=%llu)",
              instSize, (unsigned long long)traceInstr.getPC(),
              static_cast<int>(traceInstr.getInstType()),
              (unsigned long long)traceInstr.getSeqNum());
    }
    const bool compressed = instSize == 2;

    if (!traceTrainBranches && traceInstr.isAnyBranch()) {
        return compressed ? static_cast<TheISA::MachInst>(0x0001u)
                          : static_cast<TheISA::MachInst>(0x00000013u);
    }

    // Default integer mappings
    uint8_t rs1 = srcRegs.empty() ? 0 : srcRegs[0];
    uint8_t rs2 = srcRegs.size() < 2 ? 0 : srcRegs[1];
    uint8_t rd  = dstRegs.empty() ? 0 : dstRegs[0];

    // Helpers to encode PC-relative immediates for branches/jumps
    auto clamp_to_even = [](int64_t v) -> int64_t { return (v & ~1LL); };
    auto encode_b_imm = [](int32_t off) -> uint32_t {
        // off is signed, even, within [-4096, 4094]
        uint32_t imm12   = (off >> 12) & 0x1;
        uint32_t imm10_5 = (off >> 5)  & 0x3F;
        uint32_t imm4_1  = (off >> 1)  & 0xF;
        uint32_t imm11   = (off >> 11) & 0x1;
        return (imm12 << 31) | (imm10_5 << 25) | (imm4_1 << 8) | (imm11 << 7);
    };
    auto encode_j_imm = [](int32_t off) -> uint32_t {
        // off is signed, even, within [-(1<<20), (1<<20)-2]
        uint32_t bit20     = (off >> 20) & 0x1;
        uint32_t bits10_1  = (off >> 1)  & 0x3FF;
        uint32_t bit11     = (off >> 11) & 0x1;
        uint32_t bits19_12 = (off >> 12) & 0xFF;
        return (bit20 << 31) | (bits19_12 << 12) | (bit11 << 20) | (bits10_1 << 21);
    };

    auto encode_cj_imm = [](int32_t off) -> uint16_t {
        uint16_t bits = 0;
        bits |= ((off >> 11) & 0x1) << 12;
        bits |= ((off >> 4)  & 0x1) << 11;
        bits |= ((off >> 9)  & 0x3) << 9;
        bits |= ((off >> 10) & 0x1) << 8;
        bits |= ((off >> 6)  & 0x1) << 7;
        bits |= ((off >> 7)  & 0x1) << 6;
        bits |= ((off >> 3)  & 0x1) << 5;
        bits |= ((off >> 1)  & 0x3) << 3;
        bits |= ((off >> 5)  & 0x1) << 2;
        return bits;
    };

    auto encode_cb_imm = [](int32_t off) -> uint16_t {
        uint16_t bits = 0;
        bits |= ((off >> 8) & 0x1) << 12;
        bits |= ((off >> 3) & 0x3) << 10;
        bits |= ((off >> 6) & 0x3) << 5;
        bits |= ((off >> 1) & 0x3) << 3;
        bits |= ((off >> 5) & 0x1) << 2;
        return bits;
    };

    auto toCompressedReg = [](uint8_t reg) -> uint8_t {
        if (reg >= 8 && reg <= 15) {
            return reg - 8;
        }
        return reg & 0x7;
    };

    auto encode_c_addi = [](uint8_t rd_out, int imm) -> uint16_t {
        int imm6 = std::max(-32, std::min(31, imm));
        uint16_t inst = 0;
        inst |= (0b000 << 13);
        inst |= ((imm6 >> 5) & 0x1) << 12;
        inst |= (rd_out & 0x1F) << 7;
        inst |= (imm6 & 0x1F) << 2;
        inst |= 0b01;
        return inst;
    };

    auto encode_c_add = [](uint8_t rd_out, uint8_t rs2_out) -> uint16_t {
        uint16_t inst = 0;
        inst |= (0b100 << 13);
        inst |= (1 << 12);
        inst |= (rd_out & 0x1F) << 7;
        inst |= (rs2_out & 0x1F) << 2;
        inst |= 0b10;
        return inst;
    };

    auto encode_c_lw = [&](uint8_t rd_out, uint8_t rs1_out) -> uint16_t {
        uint16_t inst = 0;
        inst |= (0b010 << 13);
        inst |= (toCompressedReg(rs1_out) & 0x7) << 7;
        inst |= (toCompressedReg(rd_out) & 0x7) << 2;
        inst |= 0b00;
        return inst;
    };

    auto encode_c_sw = [&](uint8_t rs2_out, uint8_t rs1_out) -> uint16_t {
        uint16_t inst = 0;
        inst |= (0b110 << 13);
        inst |= (toCompressedReg(rs1_out) & 0x7) << 7;
        inst |= (toCompressedReg(rs2_out) & 0x7) << 2;
        inst |= 0b00;
        return inst;
    };

    auto encode_cj = [&](int32_t off, bool link) -> uint16_t {
        uint16_t inst = 0;
        inst |= ((link ? 0b001 : 0b101) << 13);
        inst |= encode_cj_imm(off);
        inst |= 0b01;
        return inst;
    };

    auto encode_cb = [&](int32_t off, bool bne, uint8_t rs1_c) -> uint16_t {
        uint16_t inst = 0;
        inst |= ((bne ? 0b111 : 0b110) << 13);
        inst |= encode_cb_imm(off);
        inst |= (rs1_c & 0x7) << 7;
        inst |= 0b01;
        return inst;
    };

    auto encode_c_jr = [](uint8_t rs1_out, bool link) -> uint16_t {
        uint16_t inst = 0;
        inst |= (0b100 << 13);
        inst |= (link ? 1 : 0) << 12;
        inst |= (rs1_out & 0x1F) << 7;
        inst |= 0b10;
        return inst;
    };

    if (compressed) {
        switch (traceInstr.getInstType()) {
            case o3::TraceInstruction::InstType::LOAD: {
                uint16_t inst = encode_c_lw(rd, rs1);
                DPRINTF(Fetch, "[TRACE-ENC] (C) load lw rs1=%u rd=%u size=%u\n", rs1, rd, instSize);
                return inst;
            }
            case o3::TraceInstruction::InstType::STORE: {
                uint16_t inst = encode_c_sw(rs2, rs1);
                DPRINTF(Fetch, "[TRACE-ENC] (C) store sw rs1=%u rs2=%u size=%u\n", rs1, rs2, instSize);
                return inst;
            }
            case o3::TraceInstruction::InstType::COND_BRANCH: {
                Addr pc = traceInstr.getPC();
                Addr tgt = traceInstr.getHasBranchTarget() ? traceInstr.getBranchTarget() : (pc + instSize);
                int64_t off64 = clamp_to_even((int64_t)tgt - (int64_t)pc);
                if (off64 < -256 || off64 > 254) {
                    int64_t clamped =
                        std::min<int64_t>(254, std::max<int64_t>(-256, off64));
                    DPRINTF(Fetch,
                            "[TRACE-ENC] (C) branch imm out of range: pc=0x%lx "
                            "tgt=0x%lx off=%lld -> clamp %lld\n",
                            (unsigned long)pc, (unsigned long)tgt,
                            (long long)off64, (long long)clamped);
                    off64 = clamped;
                }
                const bool taken = traceInstr.getBranchTaken();
                uint16_t inst = encode_cb(static_cast<int32_t>(off64), taken, toCompressedReg(rs1));
                DPRINTF(Fetch, "[TRACE-ENC] (C) cond branch pc=0x%lx tgt=0x%lx off=%lld rs1=%u taken=%d\n",
                        (unsigned long)pc, (unsigned long)tgt, (long long)off64, rs1, taken);
                return inst;
            }
            case o3::TraceInstruction::InstType::UNCOND_DIRECT_BRANCH:
            case o3::TraceInstruction::InstType::CALL_DIRECT: {
                Addr pc = traceInstr.getPC();
                Addr tgt = traceInstr.getHasBranchTarget() ? traceInstr.getBranchTarget() : (pc + instSize);
                int64_t off64 = clamp_to_even((int64_t)tgt - (int64_t)pc);
                if (off64 < -2048 || off64 > 2046) {
                    int64_t clamped =
                        std::min<int64_t>(2046, std::max<int64_t>(-2048, off64));
                    DPRINTF(Fetch,
                            "[TRACE-ENC] (C) jump imm out of range: pc=0x%lx "
                            "tgt=0x%lx off=%lld -> clamp %lld\n",
                            (unsigned long)pc, (unsigned long)tgt,
                            (long long)off64, (long long)clamped);
                    off64 = clamped;
                }
                if (traceInstr.getInstType() == o3::TraceInstruction::InstType::CALL_DIRECT) {
                    // RV64 无 c.jal，退化为 c.nop 保持长度；依赖无法保留。
                    uint16_t inst = 0x0001u;
                    DPRINTF(Fetch,
                            "[TRACE-ENC] (C) CALL_DIRECT not compressible on "
                            "RV64, fallback to c.nop pc=0x%lx\n",
                            (unsigned long)pc);
                    return inst;
                }
                uint16_t inst = encode_cj(static_cast<int32_t>(off64), false);
                DPRINTF(Fetch, "[TRACE-ENC] (C) jump type=%d pc=0x%lx tgt=0x%lx off=%lld\n",
                        static_cast<int>(traceInstr.getInstType()), (unsigned long)pc,
                        (unsigned long)tgt, (long long)off64);
                return inst;
            }
            case o3::TraceInstruction::InstType::UNCOND_INDIRECT_BRANCH:
            case o3::TraceInstruction::InstType::CALL_INDIRECT:
            case o3::TraceInstruction::InstType::RETURN: {
                uint8_t rs1_out = rs1;
                if (traceInstr.getInstType() == o3::TraceInstruction::InstType::RETURN) {
                    rs1_out = 1;
                }
                const bool link = traceInstr.getInstType() == o3::TraceInstruction::InstType::CALL_INDIRECT;
                uint16_t inst = encode_c_jr(rs1_out, link);
                DPRINTF(Fetch, "[TRACE-ENC] (C) jalr/jr type=%d rs1=%u link=%d\n",
                        static_cast<int>(traceInstr.getInstType()), rs1_out, link);
                return inst;
            }
            case o3::TraceInstruction::InstType::ALU:
            case o3::TraceInstruction::InstType::SLOW_ALU: {
                if (srcRegs.size() >= 2) {
                    // c.add/c.mv 压缩 ALU 要求 rd/rs2 非 x0，否则可能被解码为 c.ebreak。
                    if (rd == 0 || rs2 == 0) {
                        uint16_t inst = 0x0001u; // c.nop
                        DPRINTF(Fetch, "[TRACE-ENC] (C) alu not compressible (rd=%u, rs2=%u), fallback to c.nop\n",
                                rd, rs2);
                        return inst;
                    }
                    uint16_t inst = encode_c_add(rd, rs2);
                    DPRINTF(Fetch, "[TRACE-ENC] (C) alu add rs1/rd=%u rs2=%u\n", rd, rs2);
                    return inst;
                } else {
                    if (rd == 0) {
                        uint16_t inst = 0x0001u; // c.nop
                        DPRINTF(Fetch, "[TRACE-ENC] (C) alu addi not compressible (rd=0), fallback to c.nop\n");
                        return inst;
                    } else {
                        uint16_t inst = encode_c_addi(rd, 1);
                        DPRINTF(Fetch, "[TRACE-ENC] (C) alu addi rd=%u imm=1\n", rd);
                        return inst;
                    }
                }
            }
            case o3::TraceInstruction::InstType::FP: {
                // 压缩 FP 指令退化为 c.nop，避免访存副作用。
                uint16_t inst = 0x0001u;
                DPRINTF(Fetch, "[TRACE-ENC] (C) fp compressed -> c.nop (skip mem side-effects)\n");
                return inst;
            }
            default:
                // 退化为 c.nop，保持指令长度，不中断仿真。
                DPRINTF(Fetch, "[TRACE-ENC] Unsupported compressed inst type=%d at PC=0x%llx, fallback to c.nop\n",
                        static_cast<int>(traceInstr.getInstType()),
                        (unsigned long long)traceInstr.getPC());
                return static_cast<TheISA::MachInst>(0x0001u);
        }
    }

    // Create semantically appropriate RISC-V instruction using actual register mappings
    switch (traceInstr.getInstType()) {
        case o3::TraceInstruction::InstType::LOAD:
            {
                TheISA::MachInst inst = (0x000 << 20) | (rs1 << 15) | (0x2 << 12) | (rd << 7) | 0x03;
                DPRINTF(Fetch, "[TRACE-ENC] load lw rs1=%u rd=%u\n", rs1, rd);
                return inst;
            }
        case o3::TraceInstruction::InstType::STORE:
            {
                TheISA::MachInst inst = (0x00 << 25) | (rs2 << 20) | (rs1 << 15) | (0x2 << 12) | (0x00 << 7) | 0x23;
                DPRINTF(Fetch, "[TRACE-ENC] store sw rs1=%u rs2=%u\n", rs1, rs2);
                return inst;
            }
        case o3::TraceInstruction::InstType::COND_BRANCH: {
            Addr pc = traceInstr.getPC();
            Addr tgt = traceInstr.getHasBranchTarget() ? traceInstr.getBranchTarget() : (pc + instSize);
            int64_t off64 = clamp_to_even((int64_t)tgt - (int64_t)pc);
            if (off64 < -4096 || off64 > 4094) {
                DPRINTF(Fetch, "[TRACE-ENC] B-imm out of range: pc=0x%lx tgt=0x%lx off=%lld; fallback to 0\n",
                        (unsigned long)pc, (unsigned long)tgt, (long long)off64);
                off64 = 0;
            }
            uint32_t imm_enc = encode_b_imm((int32_t)off64);
            uint32_t inst = imm_enc | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (0x00 << 7) | 0x63; // BEQ
            DPRINTF(Fetch, "[TRACE-ENC] beq pc=0x%lx tgt=0x%lx off=%lld rs1=%u rs2=%u\n",
                    (unsigned long)pc, (unsigned long)tgt, (long long)off64, rs1, rs2);
            return inst;
        }
        case o3::TraceInstruction::InstType::UNCOND_DIRECT_BRANCH:
        case o3::TraceInstruction::InstType::CALL_DIRECT: {
            Addr pc = traceInstr.getPC();
            Addr tgt = traceInstr.getHasBranchTarget() ? traceInstr.getBranchTarget() : (pc + instSize);
            int64_t off64 = clamp_to_even((int64_t)tgt - (int64_t)pc);
            if (off64 < -(1<<20) || off64 > ((1<<20) - 2)) {
                DPRINTF(Fetch, "[TRACE-ENC] J-imm out of range: pc=0x%lx tgt=0x%lx off=%lld; fallback to 0\n",
                        (unsigned long)pc, (unsigned long)tgt, (long long)off64);
                off64 = 0;
            }
            uint32_t imm_enc = encode_j_imm((int32_t)off64);
            uint8_t rd_out = (traceInstr.getInstType() == o3::TraceInstruction::InstType::CALL_DIRECT) ? 1 : 0;
            uint32_t inst = imm_enc | (rd_out << 7) | 0x6F; // JAL
            DPRINTF(Fetch, "[TRACE-ENC] jal type=%d pc=0x%lx tgt=0x%lx off=%lld rd=%u\n",
                    static_cast<int>(traceInstr.getInstType()), (unsigned long)pc, (unsigned long)tgt,
                    (long long)off64, rd_out);
            return inst;
        }
        case o3::TraceInstruction::InstType::UNCOND_INDIRECT_BRANCH:
        case o3::TraceInstruction::InstType::CALL_INDIRECT:
        case o3::TraceInstruction::InstType::RETURN: {
            uint8_t rd_out = 0;
            uint8_t rs1_out = rs1;
            if (traceInstr.getInstType() == o3::TraceInstruction::InstType::CALL_INDIRECT) {
                rd_out = 1; // write RA
            } else if (traceInstr.getInstType() == o3::TraceInstruction::InstType::RETURN) {
                rd_out = 0;
                rs1_out = 1; // return from RA
            } else {
                rd_out = 0; // branch
            }
            if (traceInstr.getInstType() == o3::TraceInstruction::InstType::UNCOND_INDIRECT_BRANCH &&
                (rs1_out == 1 || rs1_out == 5)) {
                rs1_out = 3; // steer away from RA/alt-RA
            }
            TheISA::MachInst inst = (0x000 << 20) | (rs1_out << 15) | (0x0 << 12) | (rd_out << 7) | 0x67;
            DPRINTF(Fetch, "[TRACE-ENC] branch jalr type=%d rs1=%u rd=%u\n",
                    static_cast<int>(traceInstr.getInstType()), rs1_out, rd_out);
            return inst;
        }
        case o3::TraceInstruction::InstType::FP:
            {
                uint8_t frd = dstRegs.empty() ? 0 : dstRegs[0];
                uint8_t frs1 = srcRegs.empty() ? 0 : srcRegs[0];
                uint8_t frs2 = srcRegs.size() < 2 ? 0 : srcRegs[1];
                TheISA::MachInst inst = (0x00 << 25) | (frs2 << 20) | (frs1 << 15) | (0x0 << 12) | (frd << 7) | 0x53;
                DPRINTF(Fetch, "[TRACE-ENC] fp fadd rs1=f%u rs2=f%u rd=f%u\n", frs1, frs2, frd);
                return inst;
            }
        case o3::TraceInstruction::InstType::ALU:
        case o3::TraceInstruction::InstType::SLOW_ALU:
        default:
            if (srcRegs.size() >= 2) {
                TheISA::MachInst inst = (0x00 << 25) | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x33;
                DPRINTF(Fetch, "[TRACE-ENC] sn=? type=ALU rs1=%u rs2=%u rd=%u\n", rs1, rs2, rd);
                return inst;
            } else {
                TheISA::MachInst inst = (0x001 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x13;
                DPRINTF(Fetch, "[TRACE-ENC] sn=? type=ALU-imm rs1=%u rd=%u imm=1\n", rs1, rd);
                return inst;
            }
    }
}

} // namespace o3
} // namespace gem5
