
#include <cassert>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"

#define private public
#include "cpu/o3/tea.hh"

#undef private
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/rob.hh"
#include "params/BaseO3CPU.hh"

using namespace gem5;
using namespace gem5::o3;
DynInstPtr inst(unsigned seq, Addr pc = 0x1000, unsigned psv = 0, ThreadID tid = 0) {
    return new DynInst(seq, pc, psv, tid);
}
struct Fixture
{
    CPU cpu;
    ROB rob;
    BaseO3CPUParams params;
    Tea tea;
    std::list<ThreadID> active{0};
    Fixture(unsigned period = 1) : tea(&cpu, configure(period)) {
        tea.setROB(&rob);
        tea.setActiveThreads(&active);
    }
    const BaseO3CPUParams& configure(unsigned period) {
        params.teaSamplePeriod = period;
        return params;
    }
    void dump(const std::string &name) {
        tea.stats.preDumpStats();
        std::filesystem::rename("test.tea.csv", name+".csv");
        std::ofstream out(name+".states.csv");
        out << "state,weight\n";
        const char *names[] = {"Computing", "Stalled", "Flushed", "Drained"};
        double states = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const double value = tea.stats.samplesByState[i].value();
            states += value;
            out << names[i] << ',' << value << '\n';
        }
        assert(states == tea.stats.samples.value());
        assert(tea.stats.samples.value() == tea.stats.attributedSamples.value() +
            tea.stats.cancelledTargets.value() + tea.stats.pendingSamples.value() +
            tea.stats.unresolvedTargets.value());
        std::ofstream counts(name+".counts.csv");
        counts << "counter,value\n";
        counts << "samples," << tea.stats.samples.value() << '\n';
        counts << "attributed," << tea.stats.attributedSamples.value() << '\n';
        counts << "cancelled," << tea.stats.cancelledTargets.value() << '\n';
        counts << "pending," << tea.stats.pendingSamples.value() << '\n';
        counts << "unresolved," << tea.stats.unresolvedTargets.value() << '\n';
        for (unsigned tid = 0; tid < params.numThreads; ++tid)
            counts << "retired" << tid << ',' << tea.stats.committedByThread[tid].value() << '\n';
        for (unsigned event = 0; event < TeaEvent::NumEvents; ++event)
            counts << "event" << event << ',' << tea.stats.samplesByEvent[event].value() << '\n';
    }
};
int main() {
    {
        Fixture f; auto a = inst(1);
        f.tea.stall(0, a); f.tea.tick();
        f.tea.commit(a); f.tea.tick();
        f.dump("collision");
    }
    {
        Fixture f; auto a = inst(1);
        f.tea.stall(0, a); f.tea.tick();
        a->memory = true; f.tea.cacheMiss(a, 3); a->teaSetEvent(TeaEvent::ST_TLB);
        f.tea.commit(a); f.tea.tick();
        f.dump("late-events");
    }
    {
        Fixture f; auto a = inst(1);
        f.tea.stall(0, a); f.tea.tick();
        f.tea.squash(a); f.dump("cancelled");
    }
    {
        Fixture f; auto a = inst(1);
        f.tea.stall(0, a); f.tea.tick();
        f.rob.empty[0] = true; f.tea.tick();
        f.dump("pending");
    }
    {
        Fixture f;
        f.tea.tick();
        f.tea.setFlush(0, 0, 0, false); f.tea.tick();
        f.dump("unresolved");
    }
    {
        Fixture f; auto a = inst(1, 0x1000, 8);
        f.tea.setFlush(0, 0x1000, 8); f.tea.tick();
        f.tea.allocate(a); f.rob.empty[0] = true; f.tea.tick();
        f.tea.commit(a); f.tea.tick(); f.dump("drain-flush");
    }
    {
        Fixture f; auto a = inst(1), b = inst(2, 0x2000);
        f.tea.commit(a); f.tea.commit(b); f.tea.tick();
        f.dump("split-commits");
    }
    {
        Fixture f; auto a = inst(1);
        f.tea.stall(0, a); f.tea.tick();
        f.tea.stats.resetStats();
        f.tea.commit(a); f.tea.tick(); f.dump("reset");
    }
    {
        Fixture f(3); auto a = inst(1);
        f.tea.stall(0, a); f.tea.tick();
        f.tea.commit(a); f.tea.tick();
        f.tea.tick();
        f.tea.commit(inst(2)); f.tea.tick(); f.dump("period3");
    }
    {
        Fixture f; f.active.push_back(1);
        f.tea.commit(inst(1));
        auto b = inst(2, 0x1000, 0, 1);
        f.tea.stall(1, b); f.tea.tick();
        f.active = {1}; f.tea.commit(b); f.tea.tick();
        f.dump("thread-isolation");
    }
    {
        Fixture f; auto a = inst(1), b = inst(2, 0x2000);
        a->micro = b->micro = true; b->last = true;
        a->events = 64;
        f.tea.commit(a); f.tea.commit(b); f.tea.tick();
        f.dump("micro-commit");
    }
    {
        Fixture f; auto a = inst(1), b = inst(2, 0x2000);
        f.tea.stall(0, a); f.tea.tick();
        f.tea.commit(a); f.tea.commit(b); f.tea.tick();
        f.tea.commit(inst(3, 0x2000)); f.tea.tick();
        f.dump("ambiguity-a");
    }
    {
        Fixture f; auto a = inst(1, 0x2000), b = inst(2);
        f.tea.stall(0, a); f.tea.tick();
        f.tea.commit(a); f.tea.commit(b); f.tea.tick();
        f.tea.commit(inst(3)); f.tea.tick();
        f.dump("ambiguity-b");
    }
}
