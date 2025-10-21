/***************************************************************************************
* Copyright (c) 2025 Institute of Computing Technology, Chinese Academy of Sciences
 * Copyright (c) 2025 Beijing Institute of Open Source Chip (BOSC)
 *
 * XiangShan is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * See the Mulan PSL v2 for more details.
 *
 *
 * Acknowledgement
 *
 * This implementation is inspired by several key papers:
 * [1] Robert. M. Tomasulo. "[An efficient algorithm for exploiting multiple arithmetic units.]
 * (https://doi.org/10.1147/rd.111.0025)" IBM Journal of Research and Development (IBMJ) 11.1: 25-33. 1967.
 ***************************************************************************************/

#include <optional>
#include <sstream>

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "debug/Fetch.hh"
#include "debug/SimFetch.hh"
#include "simFetch.hh"

namespace gem5
{
namespace o3
{

SimFetch::SimFetch(const std::string& trace_path)
    : Named("SimFetch"),
      fetchFinish(false),
      traceReaderIdx(0), traceQueueEnqIdx(0), traceQueueDeqIdx(0), traceQueueReadIdx(0),
      ftqEnqIdx(0), ftqDeqIdx(0), ftqReadIdx(0), fetchHead(0),
      needNewFtqEntry(true)
{
    traceStream.open(trace_path);
    if (!traceStream.is_open()) {
        fatal("Could not open trace file for SimFetch: %s", trace_path);
    }
}

bool
SimFetch::fillNextFTQ()
{
    uint32_t current_ftq_inst_bytes = 0;
    uint32_t current_inst_bytes = 0;
    uint32_t current_ftq_inst_num = 0;

    if (ftq.size() < maxFtqSize) {

        bool needNewFtq = false;
        uint32_t current_ftq_first_trace_queue_idx = traceQueueEnqIdx;

        while (!needNewFtq) {
            if (fetchFinish) return true;
            auto single_trace = readerAndParseNextTrace();

            current_inst_bytes = isRvC(single_trace.inst) ? 2 : 4;
            current_ftq_inst_bytes += current_inst_bytes;

            if (current_ftq_inst_bytes > maxFtqBytes) {
                fatal("One ftq entry has overflowed!");
            }

            Addr continuous_next_pc = single_trace.pc + current_inst_bytes;

            // TODO If the next PC after the branch is identical to the next PC in sequence,
            // TODO it should currently be unprocessable.
            auto next_trace = readerAndParseNextTrace(true);
            uint32_t next_inst_bytes = isRvC(next_trace.inst) ? 2 : 4;
            // taken, jump, exception
            bool is_discontinuous = continuous_next_pc != next_trace.pc;
            bool is_jump_or_taken = (is_discontinuous && single_trace.instInfo.brType != BrType::NotCfi) ||
                                    single_trace.instInfo.isJump;

            if (is_jump_or_taken) {
                DPRINTF(SimFetch, "set jump or taken, pc: 0x%x\n", single_trace.pc);
            }

            Addr next_pc = is_jump_or_taken ? next_trace.pc : continuous_next_pc;
            needNewFtq = is_discontinuous || current_ftq_inst_bytes + next_inst_bytes > 32 || is_jump_or_taken;

            single_trace.isJumpOrTaken = is_jump_or_taken;
            single_trace.npc = next_pc;
            single_trace.ftqIdx = ftqEnqIdx;
            single_trace.isLastFtqEntry = needNewFtq;

            traceQueuEnq(single_trace);
            current_ftq_inst_num++;
        }

        FTQEntry ftq_entry = {current_ftq_first_trace_queue_idx, current_ftq_inst_num};
        ftqEnq(ftq_entry);

        return true;
    }

    return false;
}

bool
SimFetch::isRvC(uint32_t instr)
{
        return (instr & 0x3) != 0x3;
}

BrType
SimFetch::getBranchType(uint32_t instr, bool rvc)
{
    {
        if (rvc) {
            uint32_t opcode = instr & 0x3;
            uint32_t funct3 = (instr >> 13) & 0x7;

            if (opcode == 0b01) {
                if (funct3 == 0b101)
                    return BrType::Jal;
                if (funct3 == 0b110 || funct3 == 0b111)
                    return BrType::Branch; // C.BEQZ, C.BNEZ
            } else if (opcode == 0b10) {
                uint32_t funct5 = (instr >> 2) & 0x1F;
                if (funct3 == 0b100 && funct5 == 0b00000)
                    return BrType::Jalr;
            }
        } else {
            uint32_t opcode = instr & 0x7F;
            uint32_t funct3 = (instr >> 12) & 0x7;
            if (opcode == 0b1100011)
                return BrType::Branch; // B-type instructions
            if (opcode == 0b1101111)
                return BrType::Jal; // JAL
            if (opcode == 0b1100111 && funct3 == 0b000)
                return BrType::Jalr; // JALR
        }
        return BrType::NotCfi;
    }
}

RiscvInstructionInfo
SimFetch::analyzeInstr(uint32_t inst)
{
    RiscvInstructionInfo info;

    info.isRVC = isRvC(inst);
    info.brType = getBranchType(inst, info.isRVC);

    if (info.isRVC) {
        info.rd = inst >> 12 & 0x1;
        if (info.brType == BrType::Jal) {
            info.rs1 = 0;
        } else {
            info.rs1 = (inst >> 7) & 0x1F;
        }
    } else {
        info.rd = (inst >> 7) & 0x1F;
        info.rs1 = (inst >> 15) & 0x1F;
    }

    bool is_jal_not_rvc = (info.brType == BrType::Jal && !info.isRVC);
    bool is_jalr = info.brType == BrType::Jalr;

    info.isCall = (is_jal_not_rvc || is_jalr) && (info.rd == 1 || info.rd == 5);
    info.isRet = (info.brType == BrType::Jalr) && (info.rs1 == 1 || info.rs1 == 5) && !info.isCall;

    info.isJump = info.brType == BrType::Jal || info.brType == BrType::Jalr;
    return info;
}
void
SimFetch::traceQueuEnq(TraceInfo trace_info)
{
    DPRINTF(SimFetch, "trace queue enq, pc: 0x%lx, instr: 0x%x, ftq id: %lu, tracequeue id: %lu\n",
        trace_info.pc, trace_info.inst, trace_info.ftqIdx, traceQueueEnqIdx);
    traceQueue.emplace(traceQueueEnqIdx, trace_info);
    traceQueueEnqIdx++;
}

void
SimFetch::ftqEnq(FTQEntry ftq_entry)
{
    ftq.emplace(ftqEnqIdx, ftq_entry);
    ftqEnqIdx++;
}

std::istream&
SimFetch::peek_line(std::istream& is, std::string& line)
{
    std::streampos original_pos = is.tellg();

    if (std::getline(is, line)) {
        is.seekg(original_pos);
    }

    return is;
}


std::optional<TraceInfo>
SimFetch::getTraceInfo(bool is_peek)
{
    auto it = traceQueue.find(traceQueueReadIdx);
    if (it != traceQueue.end()) {
        if (!is_peek) traceQueueReadIdx++;
        return it->second;
    } else {
        return std::nullopt;
    }
}

FTQEntry
SimFetch::getFTQEntry(bool is_peek)
{
    auto ftq_entry = ftq[ftqReadIdx];
    if (!is_peek) ftqDeqIdx++;
    return ftq_entry;
}

void
SimFetch::advanceFtqReadIdx(uint32_t count)
{
    ftqReadIdx += count;
}

void
SimFetch::advanceTraceQueueReadIdx(uint32_t count)
{
    traceQueueReadIdx += count;
}

bool
SimFetch::redirect(uint64_t ftq_idx, uint64_t trace_queue_idx, Addr pc, bool type)
{
    DPRINTF(SimFetch, "[Anzo] redirect ftq_idx: %lu, trace_queue_idx: %lu, pc: 0x%lx, type: % d\n",
        ftq_idx, trace_queue_idx, pc, type);
    auto trace_queue_it = traceQueue.find(trace_queue_idx);
    auto ftq_it = ftq.find(ftq_idx);

    if (trace_queue_it == traceQueue.end()) {
        DPRINTF(SimFetch, "trace queue finish\n");
    }

    if (ftq_it == ftq.end()) {
        DPRINTF(SimFetch, "ftq finish\n");
    }

    if (trace_queue_it != traceQueue.end() && ftq_it != ftq.end()) {
        // type == 1 先找下一个，再找自己的 ftq。
        // 用于分支预测错误/异常
        if (type == 1) {
            bool q_q_match = trace_queue_it->second.ftqIdx == ftq_idx;
            bool is_last_trace_entry = trace_queue_it->second.isLastFtqEntry;
            if (q_q_match) {
                assert(ftq_idx <= ftqReadIdx && trace_queue_idx <= traceQueueReadIdx);
                uint64_t target_trace_queue_idx = trace_queue_idx;
                uint64_t target_ftq_idx = ftq_idx;

                Addr trace_queue_pc = trace_queue_it->second.pc;
                DPRINTF(SimFetch, "trace queue pc: 0x%lx, redirect pc: 0x%lx\n",
                    trace_queue_pc, pc);

                if (trace_queue_pc == pc) {
                    traceQueueReadIdx = target_trace_queue_idx;
                    ftqReadIdx = target_ftq_idx;

                    return true;
                }

                // TODO
                // 我认为这是一个taken的指令，但是不知道怎么的，给错了。
                if (is_last_trace_entry) {
                    target_ftq_idx = ftq_idx + 1;
                    auto next_ftq_it = ftq.find(target_ftq_idx);
                    if (next_ftq_it != ftq.end()) {
                        target_trace_queue_idx = next_ftq_it->second.firstTraceIdx;
                    }
                } else {
                    // 我不认为这是一个taken的，所有没有分ftq，例如：分支的下一条就是顺序的下一条。
                    target_trace_queue_idx = trace_queue_idx + 1;
                    target_ftq_idx = ftq_idx;
                }

                Addr next_trace_pc = traceQueue[target_trace_queue_idx].pc;
                DPRINTF(SimFetch, "redirect expect ftq id: %lu, tracequeue id: %lu, pc: 0x%lx\n",
                    target_ftq_idx, target_trace_queue_idx, next_trace_pc);

                if (next_trace_pc == pc) {
                    traceQueueReadIdx = target_trace_queue_idx;
                    ftqReadIdx = target_ftq_idx;

                    return true;
                }
            } else {
                fatal("q q mismatch! ftq idx: %lu, from tracequeue ftd idx: %lu\n",
                    ftq_idx, trace_queue_it->second.ftqIdx);
            }
        } else {
            auto ftq_it = ftq.find(ftq_idx);
            if (ftq_it != ftq.end()) {
                uint64_t trace_queue_start_idx = ftq_it->second.firstTraceIdx;
                uint32_t ftq_nums = ftq_it->second.numInsts;
                for (int i = 0; i < ftq_nums; ++i) {
                    if (traceQueue[trace_queue_start_idx + i].pc == pc) {
                        traceQueueReadIdx = trace_queue_start_idx + i;
                        ftqReadIdx = ftq_idx;

                        return true;
                    }
                }
            }
        }
    }

    return false;
}

bool
SimFetch::commit(uint64_t trace_queue_idx)
{
    DPRINTF(SimFetch, "Commit sim trace queue started size: 0x%lx, commit idx: 0x%lx\n",
        traceQueue.size(), trace_queue_idx);

    auto find_it = traceQueue.find(trace_queue_idx);

    auto ftq_begin_idx = ftq.end()->first;
    auto commit_ftq_idx = find_it->second.ftqIdx;

    if (find_it->second.isLastFtqEntry) {
        auto ftq_end_it = ftq.upper_bound(find_it->second.ftqIdx);
        ftq.erase(ftq.begin(), ftq_end_it);
    } else if (ftq_begin_idx < commit_ftq_idx) {
        auto ftq_end_it = ftq.upper_bound(find_it->second.ftqIdx - 1);
        ftq.erase(ftq.begin(), ftq_end_it);
    }

    if (find_it != traceQueue.end()) {
        auto trace_queue_end_it = traceQueue.upper_bound(trace_queue_idx);
        traceQueue.erase(traceQueue.begin(), trace_queue_end_it);

        DPRINTF(SimFetch, "Commit sim trace queue finished size: 0x%lx, commit count: 0x%lx\n",
            traceQueue.size(), traceQueue.begin()->first);
        return true;
    }

    return false;
}

TraceInfo
SimFetch::readerAndParseNextTrace(bool is_peek)
{
    TraceInfo single_trace;

    std::string line;

    if (is_peek) {
        if (!peek_line(traceStream, line) || line.empty()) {
            fetchFinish = true;
            memset(&single_trace, 0, sizeof(single_trace));
            return single_trace;
            fatal("The trace of the next line cannot be peek");
        }
    } else {
        if (!std::getline(traceStream, line) || line.empty()) {
            fetchFinish = true;
            memset(&single_trace, 0, sizeof(single_trace));
            return single_trace;
        }
    }

    std::stringstream ss(line);
    std::string pc_str, inst_str;
    std::getline(ss, pc_str, ':');
    std::getline(ss, inst_str);

    single_trace.pc = std::stoull(pc_str, nullptr, 16);
    single_trace.npc = single_trace.pc;
    single_trace.inst = std::stoull(inst_str, nullptr, 16);
    single_trace.instInfo = analyzeInstr(single_trace.inst);
    single_trace.ftqIdx = ftqEnqIdx;
    single_trace.isLastFtqEntry = false;
    single_trace.isJumpOrTaken = false;
    return single_trace;
}

}
}
