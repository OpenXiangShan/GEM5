/*
 * Copyright (c) 2012 Google
 * Copyright (c) 2017 The University of Virginia
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

#ifndef __ARCH_RISCV_DECODER_HH__
#define __ARCH_RISCV_DECODER_HH__

#include <deque>

#include "arch/generic/decode_cache.hh"
#include "arch/generic/decoder.hh"
#include "arch/riscv/insts/vector.hh"
#include "arch/riscv/types.hh"
#include "arch/riscv/vtype_pred.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/static_inst.hh"
#include "debug/Decode.hh"
#include "params/RiscvDecoder.hh"

namespace gem5
{

namespace RiscvISA
{

class ISA;
class Decoder : public InstDecoder
{
  protected:
    //The extended machine instruction being generated
    ExtMachInst emi;
    uint32_t machInst;

    bool vtypeReady = true;
    VTYPE machVtype = (uint64_t)1 << 63;  // default vtype illegal

    // Known VL tracking for micro-op splitting optimization
    bool vlIsKnown = false;
    uint32_t machVl = 0;

    // vtype prediction state
    VtypePredictor vtypePred;
    bool    vtypeIsPredicted  = false;
    uint8_t predictedVtypeVal = 0;

    struct VectorStateCheckpoint
    {
        InstSeqNum seqNum = 0;
        VTYPE vtype = 0;
        uint32_t vl = 0;
        bool vlKnown = false;
    };

    std::deque<VectorStateCheckpoint> vectorStateHistory;

    /// A cache of decoded instruction objects.
    static GenericISA::BasicDecodeCache<Decoder, ExtMachInst> defaultCache;
    friend class GenericISA::BasicDecodeCache<Decoder, ExtMachInst>;

    StaticInstPtr decodeInst(ExtMachInst mach_inst);

    /// Decode a machine instruction.
    /// @param mach_inst The binary instruction to decode.
    /// @retval A pointer to the corresponding StaticInst object.
    StaticInstPtr decode(ExtMachInst mach_inst, Addr addr);

  public:
    Decoder(const RiscvDecoderParams &p)
        : InstDecoder(p, &machInst),
          vtypePred(p.vtype_pred_entries)
    {
        reset();
    }

    void reset() override;

    /** Returns true when the last decoded VectorConfig used a speculative
     *  vtype prediction rather than stalling fetch. */
    bool isVtypePredicted() const { return vtypeIsPredicted; }

    /** Returns the predicted vtype value that was speculatively used. */
    uint8_t getPredictedVtypeVal() const { return predictedVtypeVal; }

    inline bool compressed(ExtMachInst inst) { return (inst & 0x3) < 0x3; }
    inline bool vconf(ExtMachInst inst) {
      return inst.opcode7 == 0b1010111u && inst.width == 0b111u;
    }

    //Use this to give data to the decoder. This should be used
    //when there is control flow.
    void moreBytes(const PCStateBase &pc, Addr fetchPC) override;

    StaticInstPtr decode(PCStateBase &nextPC) override;


    void setPCStateWithInstDesc(const bool &inst,
                                  PCStateBase &pc) override;

    void setVtype(VTYPE vtype);

    void setVectorState(VTYPE vtype, uint32_t vl, bool vl_known = true);

    void checkpointVectorState(InstSeqNum seq_num);
    void updateVectorStateCheckpoint(InstSeqNum seq_num, VTYPE vtype,
                                     uint32_t vl, bool vl_known);
    void commitVectorStateCheckpoints(InstSeqNum seq_num);
    void rollbackVectorState(InstSeqNum seq_num, VTYPE committed_vtype,
                             uint32_t committed_vl, bool committed_vl_known);

    void updateKnownVl(ExtMachInst emi, int earlyVtype);

    void clearVtype();

    bool stall() override;

    /**
     * Attempt to predict vtype for a register-form vsetvl at @p pc.
     * Returns true and writes the prediction into @p pred when a valid
     * entry exists. Caller should call setVtype(pred) on success and
     * clearVtype() on failure.
     */
    bool tryPredictVtype(Addr pc, uint8_t &pred)
    {
        return vtypePred.predict(pc, pred);
    }

    /** Record the confirmed vtype for the vsetvl at @p pc. */
    void updateVtypePredictor(Addr pc, uint8_t vtype)
    {
        vtypePred.update(pc, vtype);
    }
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_DECODER_HH__
