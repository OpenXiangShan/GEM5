
#pragma once
#include "cpu/o3/tea.hh"

namespace gem5 { namespace o3 {
struct PC
{
    Addr address; Addr instAddr() const { return address; } };
class DynInst
{
public:
    ThreadID threadNumber;
    InstSeqNum seqNum;
    PC pc;
    uint16_t events;
    bool micro = false, last = false, memory = false;
    std::vector<DynInst*> fused;
    DynInst(unsigned seq, Addr address, unsigned psv = 0, ThreadID tid = 0)
        : threadNumber(tid), seqNum(seq), pc{address}, events(psv) {}
    const DynInst *teaFirstMember() const { return fused.empty() ? this : fused.front(); }
    const PC& pcState() const { return pc; }
    Addr getPC() const { return pc.address; }
    uint16_t teaPSV() const { return events; }
    void teaSetEvent(TeaEvent::Event event) { events |= TeaEvent::bit(event); }
    bool isLoad() const { return memory; }
    bool isMemRef() const { return memory; }
    bool isMicroop() const { return micro; }
    bool isLastMicroop() const { return last; }
    bool isFusion() const { return !fused.empty(); }
    const std::vector<DynInst*>& teaFusionMembers() const { return fused; }
};
}}
