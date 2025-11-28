/*
 * Copyright (c) 2024 The Regents of The University of Michigan
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

#ifndef __CPU_O3_TRACE_TRACE_INSTRUCTION_HH__
#define __CPU_O3_TRACE_TRACE_INSTRUCTION_HH__

#include <cstdint>
#include <vector>

#include "base/types.hh"

namespace gem5
{
namespace o3
{

/**
 * Unified instruction representation for trace-driven simulation.
 * This class provides a common interface for instructions read from
 * different trace formats (ChampSim, CBP2025, etc.) to be consumed
 * by the O3CPU fetch stage.
 */
class TraceInstruction
{
  public:
    /** Instruction type enumeration */
    enum class InstType : uint8_t
    {
        ALU = 0,
        LOAD = 1,
        STORE = 2,
        COND_BRANCH = 3,
        UNCOND_DIRECT_BRANCH = 4,
        UNCOND_INDIRECT_BRANCH = 5,
        FP = 6,
        SLOW_ALU = 7,
        CALL_DIRECT = 8,
        CALL_INDIRECT = 9,
        RETURN = 10,
        UNDEFINED = 11
    };

    /** Maximum number of source/destination registers */
    static constexpr size_t MAX_SRC_REGS = 4;
    static constexpr size_t MAX_DST_REGS = 4;

  private:
    /** Program counter */
    Addr pc;

    /** Instruction type */
    InstType instType;

    /** Branch-specific information */
    bool isBranch;
    bool branchTaken;
    bool hasBranchTarget;
    Addr branchTarget;

    /** Non-branch control-flow change information (e.g., traps/exceptions) */
    bool ctrlFlowChange;
    bool hasCtrlFlowTarget;
    Addr ctrlFlowTarget;

    /** Memory operation information */
    bool isLoad;
    bool isStore;
    std::vector<Addr> loadAddresses;
    std::vector<Addr> storeAddresses;
    std::vector<uint32_t> memSizes;

    /** Memory values from trace for cache simulation trick */
    std::vector<uint64_t> loadValues;    // Values loaded from memory in trace
    std::vector<uint64_t> storeValues;   // Values stored to memory in trace

    /** Register dependency information */
    std::vector<uint8_t> srcRegs;
    std::vector<uint8_t> dstRegs;

    /** Unique sequence number for tracking */
    uint64_t seqNum;
    uint8_t piece;  // For CBP2025 multi-piece instructions

    /** Additional instruction metadata for O3CPU */
    bool hasImmediateOperand;
    uint64_t immediateValue;

    /** Instruction size in bytes (e.g., 2 for compressed, 4 for standard) */
    uint8_t instSizeBytes;

    /** Additional metadata */
    bool valid;

    /** Whether this is the last instruction in the trace stream */
    bool lastInTrace;

  public:
    /** Default constructor */
    TraceInstruction() :
        pc(0), instType(InstType::UNDEFINED), isBranch(false),
        branchTaken(false), hasBranchTarget(false), branchTarget(0),
        ctrlFlowChange(false), hasCtrlFlowTarget(false), ctrlFlowTarget(0),
        isLoad(false), isStore(false),
        seqNum(0), piece(0), hasImmediateOperand(false), immediateValue(0),
        instSizeBytes(4),
        valid(false), lastInTrace(false) {}

    /** Constructor with basic fields */
    TraceInstruction(Addr _pc, InstType _type, uint64_t _seqNum = 0) :
        pc(_pc), instType(_type), isBranch(false), branchTaken(false),
        hasBranchTarget(false), branchTarget(0),
        ctrlFlowChange(false), hasCtrlFlowTarget(false), ctrlFlowTarget(0),
        isLoad(_type == InstType::LOAD),
        isStore(_type == InstType::STORE),
        seqNum(_seqNum), piece(0), hasImmediateOperand(false), immediateValue(0),
        instSizeBytes(4),
        valid(true), lastInTrace(false)
    {
        isBranch = (_type == InstType::COND_BRANCH ||
                   _type == InstType::UNCOND_DIRECT_BRANCH ||
                   _type == InstType::UNCOND_INDIRECT_BRANCH ||
                   _type == InstType::CALL_DIRECT ||
                   _type == InstType::CALL_INDIRECT ||
                   _type == InstType::RETURN);
    }

    // Getters
    Addr getPC() const { return pc; }
    InstType getInstType() const { return instType; }
    bool getBranch() const { return isBranch; }
    bool getBranchTaken() const { return branchTaken; }
    bool getHasBranchTarget() const { return hasBranchTarget; }
    Addr getBranchTarget() const { return branchTarget; }
    bool getLoad() const { return isLoad; }
    bool getStore() const { return isStore; }
    const std::vector<Addr>& getLoadAddresses() const { return loadAddresses; }
    const std::vector<Addr>& getStoreAddresses() const { return storeAddresses; }
    const std::vector<uint32_t>& getMemSizes() const { return memSizes; }
    const std::vector<uint64_t>& getLoadValues() const { return loadValues; }
    const std::vector<uint64_t>& getStoreValues() const { return storeValues; }
    const std::vector<uint8_t>& getSrcRegs() const { return srcRegs; }
    const std::vector<uint8_t>& getDstRegs() const { return dstRegs; }
    uint64_t getSeqNum() const { return seqNum; }
    uint8_t getPiece() const { return piece; }
    bool isValid() const { return valid; }
    uint8_t getInstSizeBytes() const { return instSizeBytes; }

    // Additional getters for O3CPU
    bool getHasImmediateOperand() const { return hasImmediateOperand; }
    uint64_t getImmediateValue() const { return immediateValue; }

    bool isLastInTrace() const { return lastInTrace; }

    // Non-branch control-flow change getters
    bool isCtrlFlowChange() const { return ctrlFlowChange; }
    bool getHasCtrlFlowTarget() const { return hasCtrlFlowTarget; }
    Addr getCtrlFlowTarget() const { return ctrlFlowTarget; }

    // Setters
    void setPC(Addr _pc) { pc = _pc; }
    void setInstType(InstType _type) {
        instType = _type;
        isLoad = (_type == InstType::LOAD);
        isStore = (_type == InstType::STORE);
        isBranch = (_type == InstType::COND_BRANCH ||
                   _type == InstType::UNCOND_DIRECT_BRANCH ||
                   _type == InstType::UNCOND_INDIRECT_BRANCH ||
                   _type == InstType::CALL_DIRECT ||
                   _type == InstType::CALL_INDIRECT ||
                   _type == InstType::RETURN);
    }
    void setBranchTaken(bool taken) { branchTaken = taken; }
    void setBranchTarget(Addr target) {
        branchTarget = target;
        hasBranchTarget = true;
    }

    // Non-branch control-flow change setters
    void setCtrlFlowChange(bool v) { ctrlFlowChange = v; }
    void setCtrlFlowTarget(Addr target)
    {
        ctrlFlowTarget = target;
        hasCtrlFlowTarget = true;
    }
    void addLoadAddress(Addr addr, uint32_t size = 0) {
        loadAddresses.push_back(addr);
        if (size > 0) memSizes.push_back(size);
    }
    void addStoreAddress(Addr addr, uint32_t size = 0) {
        storeAddresses.push_back(addr);
        if (size > 0) memSizes.push_back(size);
    }
    void addLoadValue(uint64_t value) {
        loadValues.push_back(value);
    }
    void addStoreValue(uint64_t value) {
        storeValues.push_back(value);
    }
    void addSrcReg(uint8_t reg) {
        if (srcRegs.size() < MAX_SRC_REGS) srcRegs.push_back(reg);
    }
    void addDstReg(uint8_t reg) {
        if (dstRegs.size() < MAX_DST_REGS) dstRegs.push_back(reg);
    }
    void setSeqNum(uint64_t _seqNum) { seqNum = _seqNum; }
    void setPiece(uint8_t _piece) { piece = _piece; }
    void setInstSizeBytes(uint8_t size) { instSizeBytes = size; }
    void setValid(bool _valid) { valid = _valid; }

    Addr getFallThroughPC() const { return pc + instSizeBytes; }

    void setLastInTrace(bool v) { lastInTrace = v; }

    // Additional setters for O3CPU
    void setImmediateOperand(uint64_t value) {
        immediateValue = value;
        hasImmediateOperand = true;
    }

    /** Reset instruction to invalid state */
    void reset() {
        pc = 0;
        instType = InstType::UNDEFINED;
        isBranch = false;
        branchTaken = false;
        hasBranchTarget = false;
        branchTarget = 0;
        ctrlFlowChange = false;
        hasCtrlFlowTarget = false;
        ctrlFlowTarget = 0;
        isLoad = false;
        isStore = false;
        loadAddresses.clear();
        storeAddresses.clear();
        memSizes.clear();
        loadValues.clear();
        storeValues.clear();
        srcRegs.clear();
        dstRegs.clear();
        seqNum = 0;
        piece = 0;
        hasImmediateOperand = false;
        immediateValue = 0;
        instSizeBytes = 4;
        valid = false;
        lastInTrace = false;
    }

    /** Check if this is a conditional branch */
    bool isConditionalBranch() const {
        return instType == InstType::COND_BRANCH;
    }

    /** Check if this is any type of branch */
    bool isAnyBranch() const {
        return isBranch;
    }

    /** Check if this is a memory operation */
    bool isMemoryOp() const {
        return isLoad || isStore;
    }

    /** Get string representation of instruction type */
    const char* getInstTypeStr() const {
        switch (instType) {
            case InstType::ALU: return "ALU";
            case InstType::LOAD: return "LOAD";
            case InstType::STORE: return "STORE";
            case InstType::COND_BRANCH: return "COND_BRANCH";
            case InstType::UNCOND_DIRECT_BRANCH: return "UNCOND_DIRECT_BRANCH";
            case InstType::UNCOND_INDIRECT_BRANCH: return "UNCOND_INDIRECT_BRANCH";
            case InstType::FP: return "FP";
            case InstType::SLOW_ALU: return "SLOW_ALU";
            case InstType::CALL_DIRECT: return "CALL_DIRECT";
            case InstType::CALL_INDIRECT: return "CALL_INDIRECT";
            case InstType::RETURN: return "RETURN";
            case InstType::UNDEFINED: return "UNDEFINED";
            default: return "UNKNOWN";
        }
    }
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_TRACE_INSTRUCTION_HH__
