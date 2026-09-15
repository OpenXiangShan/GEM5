#!/usr/bin/env python3
"""Focused Hybrid CROB tests using current, unmodified production method bodies.

Run with Python directly; only a C++17 compiler is needed. The fixture substitutes
DynInst flag/state accessors, statistics storage and inactive CPU interfaces. It
compiles the actual ROB classifier/planner/insertion/removal methods and the
entire Commit::moveInstsToBuffer() from the working tree. No grouping, admission
or group-accounting implementation is duplicated here. This is not a full CPU
integration test: execution, decoding, Rename recovery and commitHead must also
be validated with simulator workloads. The companion gtest includes rob.hh
directly to validate the production planner in the normal build environment.
"""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


def method(source, owner, name):
    """Read one out-of-line method, preserving its complete body verbatim."""
    match = re.search(r"^([^\n]+)\n" + owner + "::" + name + r"\(",
                      source, re.MULTILINE)
    if match is None:
        raise AssertionError(f"Missing production method {owner}::{name}")
    end = source.index("\n}", match.end()) + 2
    return source[match.start():end]


PREAMBLE = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <list>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#define DPRINTF(...) ((void)0)
#define panic_if(condition, ...) assert(!(condition))
using ThreadID = unsigned;
using InstSeqNum = uint64_t;
constexpr unsigned MaxThreads = 1;
constexpr ThreadID InvalidThreadID = 999;
namespace debug { constexpr bool ROB = true; }
enum OpClass {
    IntAluOp, IntMultOp, IntDivOp, Int2FpOp, FloatAddOp, FloatMultOp,
    FloatMultAccOp, FloatDivOp, FloatSqrtOp, FloatCmpOp, FloatCvtOp,
    FloatMvOp, FloatMiscOp, IntJpOp, VectorConfigOp, UncoveredOp
};
struct Version {
    unsigned value = 0;
    bool largerThan(Version other) const { return value > other.value; }
};
struct DynInst {
    ThreadID threadNumber = 0;
    InstSeqNum seqNum = 0;
    OpClass op = IntAluOp;
    bool squashed = false, ready = true, executed = true;
    bool inROB = false, committed = false;
    Version version;
    bool isSquashed() const { return squashed; }
    void setSquashed() { squashed = true; }
    bool readyToCommit() const { return ready; }
    void setCanCommit() { ready = true; }
    bool isExecuted() const { return executed; }
    void setInROB() { inROB = true; }
    void clearInROB() { inROB = false; }
    void setCommitted() { committed = true; }
    OpClass opClass() const { return op; }
    Version getVersion() const { return version; }
#define FLAG(name) bool name##Flag = false; \
    bool name() const { return name##Flag; }
    FLAG(faulted)
    FLAG(isSerializing)
    FLAG(isSerializeBefore)
    FLAG(isSerializeAfter)
    FLAG(isNonSpeculative)
    FLAG(isSquashAfter)
    FLAG(isReadBarrier)
    FLAG(isWriteBarrier)
    FLAG(isAtomic)
    FLAG(isLoadReserved)
    FLAG(isStoreConditional)
    FLAG(isVector)
    FLAG(isMicroop)
    FLAG(isMacroop)
    FLAG(isLoad)
    FLAG(isStore)
    FLAG(isControl)
#undef FLAG
};
using DynInstPtr = std::shared_ptr<DynInst>;
struct CPU {
    unsigned cycle = 0, removed = 0;
    unsigned curCycle() const { return cycle; }
    bool isThreadExiting(ThreadID) const { return false; }
    void removeFrontInst(const DynInstPtr &) { ++removed; }
};
struct Samples {
    std::vector<unsigned> values;
    void sample(unsigned value) { values.push_back(value); }
};
struct Stats {
    unsigned writes = 0, reads = 0;
    unsigned hybridAllocatedGroups = 0, hybridAllocatedInsts = 0;
    std::array<unsigned, 6> hybridGroupType = {};
    Samples hybridGroupLength, instPergroup, smtRestEntryWhileROBFull;
    unsigned ROBFull[1] = {};
};
enum class SMTQueuePolicy { Dynamic, DynamicBorrowing };
class Commit;
class ROB {
  public:
    using InstIt = std::list<DynInstPtr>::iterator;
    bool hybrid = true;
    SMTQueuePolicy robPolicy = SMTQueuePolicy::Dynamic;
    unsigned numThreads = 1, numEntries, instsPerGroup = 8;
    unsigned numInstsInROB = 0, maxEntries[1], lastInsertCycle = 0;
    unsigned dynSquashWidth = 64;
    CPU cpuObject;
    CPU *cpu = &cpuObject;
    Stats stats;
    std::deque<unsigned> threadGroups[1];
    std::list<DynInstPtr> instList[1];
    std::list<ThreadID> threads{0};
    std::list<ThreadID> *activeThreads = &threads;
    InstIt head, tail, squashIt[1];
    InstSeqNum squashedSeqNum[1] = {};
    bool doneSquashing[1] = {true};
    explicit ROB(unsigned entries) : numEntries(entries), maxEntries{entries} {
        head = tail = squashIt[0] = instList[0].end();
    }
    bool isHybrid() const { return hybrid; }
    unsigned getTotalEntries() const { return numEntries; }
    unsigned getThreadEntries(ThreadID tid) const {
        return threadGroups[tid].size();
    }
    unsigned borrowingLimit(ThreadID tid) const { return maxEntries[tid]; }
    void setBorrowingDonor(ThreadID, bool, Commit *) {}
    void insertInst(const DynInstPtr &) { assert(false); }
"""

COMMIT_FIXTURE = r"""
enum StallReason { NoStall, OtherFragStall, ROBFull };
struct IewInfo { StallReason robHeadStallReason = NoStall; };
struct IewBuffer { IewInfo iewInfo[1]; };
bool smtHasBorrowThrottleStall(const IewInfo &) { return false; }
unsigned smtBorrowPriority(const IewInfo &) { return 0; }
struct Signals {
    bool blockIEW[1] = {};
    StallReason iewBlockReason[1] = {NoStall};
};
struct RenameBuffer { unsigned size = 0; DynInstPtr insts[8]; };
struct FixedBuffer : public std::deque<DynInstPtr> {
    bool full() const { return size() >= 256; }
};
class Commit {
  public:
    enum Status { Running, ROBSquashing, TrapPending };
    ROB *rob;
    unsigned numThreads = 1, renameWidth = 8, numPreDispatchThreads = 1;
    unsigned borrowingDonorCycles[1] = {}, smtBorrowDonorHoldCycles = 0;
    unsigned curSquashCause[1] = {};
    bool changedROBNumEntries[1] = {};
    InstSeqNum youngestSeqNum[1] = {};
    Status commitStatus[1] = {Running};
    FixedBuffer fixedbuffer[1];
    RenameBuffer renameBuffer;
    RenameBuffer *fromRename = &renameBuffer;
    Version localSquashVer[1];
    IewBuffer iewBuffer;
    IewBuffer *robInfoFromIEW = &iewBuffer;
    Signals signals;
    Signals *stallSig = &signals;
    Stats stats;
    explicit Commit(ROB &rob) : rob(&rob) {}
    StallReason squashCauseToStallReason(unsigned) { return OtherFragStall; }
    void moveInstsToBuffer();
};
"""

CASES = r"""
DynInstPtr inst(char cls, unsigned sn) {
    auto result = std::make_shared<DynInst>();
    result->seqNum = sn;
    result->op = cls == 'S' ? IntAluOp :
        cls == 'C' ? IntJpOp : UncoveredOp;
    return result;
}
std::vector<DynInstPtr> batch(const std::string &classes, unsigned sn = 1) {
    std::vector<DynInstPtr> result;
    for (char cls : classes) result.push_back(inst(cls, sn++));
    return result;
}
void enqueue(Commit &commit, const std::vector<DynInstPtr> &insts) {
    for (const auto &inst : insts) commit.fixedbuffer[0].push_back(inst);
}
void insert(ROB &rob, const std::string &classes, unsigned sn = 1) {
    const auto insts = batch(classes, sn);
    const auto plan = rob.planHybridBatch(insts);
    assert(rob.canAllocate(0, plan.size()));
    rob.insertHybridBatch(insts, plan);
}
void check(const ROB &rob, std::initializer_list<unsigned> expected) {
    assert(std::vector<unsigned>(rob.threadGroups[0].begin(),
                                rob.threadGroups[0].end()) ==
           std::vector<unsigned>(expected));
    assert(std::accumulate(expected.begin(), expected.end(), 0U) ==
           rob.instList[0].size());
    rob.assertHybridInvariants(0);
}
void one_free_entry() {
    ROB rob(2); Commit commit(rob);
    insert(rob, "N");
    assert(rob.canAllocate(0, 1));
    assert(!rob.canAllocate(0, 8));
    enqueue(commit, batch("SSSSSSSS", 2));
    commit.moveInstsToBuffer();
    check(rob, {1, 8});
    assert(commit.fixedbuffer[0].empty());
    assert(rob.stats.hybridAllocatedGroups == 2);
    assert(rob.stats.hybridAllocatedInsts == 9);
    assert(rob.stats.hybridGroupLength.values == std::vector<unsigned>({1, 8}));
    assert(rob.stats.hybridGroupType[0] == 1);
    assert(rob.stats.hybridGroupType[2] == 1);
}
void failed_admission_and_replan() {
    ROB rob(2); Commit commit(rob);
    insert(rob, "N");
    const auto candidates = batch("CCCCCCCC", 2);
    enqueue(commit, candidates);
    commit.moveInstsToBuffer();
    check(rob, {1});
    assert(commit.fixedbuffer[0].size() == 8);
    assert(rob.stats.hybridAllocatedGroups == 1);
    assert(rob.stats.hybridAllocatedInsts == 1);
    assert(rob.stats.hybridGroupLength.values.size() == 1);
    for (unsigned i = 2; i < candidates.size(); ++i)
        candidates[i]->setSquashed();
    commit.moveInstsToBuffer();
    check(rob, {1, 2});
    assert(commit.fixedbuffer[0].empty());
    assert(rob.stats.hybridAllocatedGroups == 2);
    assert(rob.stats.hybridAllocatedInsts == 3);
    assert(rob.stats.hybridGroupType[3] == 1);
}
void squash_window_no_refill() {
    ROB rob(1); Commit commit(rob);
    insert(rob, "N");
    auto candidates = batch("SSSSSSSS", 2);
    for (auto &inst : candidates) inst->setSquashed();
    enqueue(commit, candidates);
    enqueue(commit, batch("S", 10));
    commit.moveInstsToBuffer();
    check(rob, {1});
    assert(commit.fixedbuffer[0].size() == 1);
    assert(commit.fixedbuffer[0].front()->seqNum == 10);
    assert(rob.stats.hybridAllocatedGroups == 1);
    assert(rob.stats.hybridAllocatedInsts == 1);
    assert(rob.stats.hybridGroupLength.values.size() == 1);
}
void mixed_squash_window_no_refill() {
    ROB rob(2); Commit commit(rob);
    auto candidates = batch("SSSSSSSSC", 1);
    for (unsigned i : {1U, 3U, 5U, 7U}) candidates[i]->setSquashed();
    enqueue(commit, candidates);
    commit.moveInstsToBuffer();
    check(rob, {4});
    assert(commit.fixedbuffer[0].size() == 1);
    commit.moveInstsToBuffer();
    check(rob, {4, 1});
    assert(commit.fixedbuffer[0].empty());
}
void partial_retire_and_drain() {
    ROB rob(1);
    insert(rob, "SSSSSSSS");
    rob.instList[0].back()->ready = false;
    assert(!rob.isHeadGroupReady(0));
    rob.instList[0].back()->ready = true;
    assert(rob.isHeadGroupReady(0));
    for (unsigned left = 7; left > 0; --left) {
        const auto retiring = rob.instList[0].front();
        rob.retireHead(0);
        check(rob, {left});
        assert(!rob.canAllocate(0, 1));
        assert(retiring->committed && !retiring->inROB);
    }
    const auto squashed = rob.instList[0].front();
    squashed->setSquashed();
    check(rob, {1});
    assert(!rob.canAllocate(0, 1));
    rob.drainSquashedHead(0);
    check(rob, {});
    assert(!squashed->committed && !squashed->inROB);
    assert(rob.canAllocate(0, 1));
}
void partial_tail_squash() {
    ROB rob(2);
    insert(rob, "SSSSSSSS");
    const auto tail = rob.instList[0].back();
    tail->setSquashed();
    check(rob, {8});
    rob.squashedSeqNum[0] = 5;
    rob.squashIt[0] = std::prev(rob.instList[0].end());
    rob.doneSquashing[0] = false;
    rob.dynSquashWidth = 2;
    rob.doSquash(0);
    check(rob, {6});
    assert(!rob.doneSquashing[0]);
    rob.doSquash(0);
    check(rob, {5});
    assert(rob.doneSquashing[0]);
    assert(!tail->inROB && tail->isSquashed());
    insert(rob, "SSS", 9);
    check(rob, {5, 3});
    rob.squashedSeqNum[0] = 0;
    rob.squashIt[0] = std::prev(rob.instList[0].end());
    rob.dynSquashWidth = 64;
    rob.doSquash(0);
    check(rob, {});
    assert(rob.doneSquashing[0]);
    assert(rob.head == rob.instList[0].end());
    assert(rob.tail == rob.instList[0].end());
}
void classification() {
    ROB rob(8);
    using Cls = ROB::HybridInstClass;
    for (auto op : {IntAluOp, IntMultOp, IntDivOp, Int2FpOp, FloatAddOp,
                   FloatMultOp, FloatMultAccOp, FloatDivOp, FloatSqrtOp,
                   FloatCmpOp, FloatCvtOp, FloatMvOp, FloatMiscOp}) {
        auto i = inst('S', 1); i->op = op;
        assert(rob.classifyHybridInst(i) == Cls::Simple);
    }
    for (auto flag : {&DynInst::isLoadFlag, &DynInst::isStoreFlag,
                     &DynInst::isControlFlag}) {
        auto i = inst('S', 1); i.get()->*flag = true;
        assert(rob.classifyHybridInst(i) == Cls::Complex);
    }
    for (auto flag : {&DynInst::faultedFlag, &DynInst::isSerializingFlag,
                     &DynInst::isSerializeBeforeFlag,
                     &DynInst::isSerializeAfterFlag,
                     &DynInst::isNonSpeculativeFlag, &DynInst::isSquashAfterFlag,
                     &DynInst::isReadBarrierFlag, &DynInst::isWriteBarrierFlag,
                     &DynInst::isAtomicFlag, &DynInst::isLoadReservedFlag,
                     &DynInst::isStoreConditionalFlag, &DynInst::isVectorFlag,
                     &DynInst::isMicroopFlag, &DynInst::isMacroopFlag}) {
        auto i = inst('C', 1); i.get()->*flag = true;
        i->isLoadFlag = true;
        assert(rob.classifyHybridInst(i) == Cls::NoCompress);
    }
    auto i = inst('S', 1); i->op = VectorConfigOp;
    assert(rob.classifyHybridInst(i) == Cls::NoCompress);
    assert(rob.classifyHybridInst(inst('C', 1)) == Cls::Complex);
    assert(rob.classifyHybridInst(inst('N', 1)) == Cls::NoCompress);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string test = argv[1];
#define CASE(name) if (test == #name) { name(); return 0; }
    CASE(one_free_entry)
    CASE(failed_admission_and_replan)
    CASE(squash_window_no_refill)
    CASE(mixed_squash_window_no_refill)
    CASE(partial_retire_and_drain)
    CASE(partial_tail_squash)
    CASE(classification)
    assert(false);
}
"""


def harness():
    rob_hh = (ROOT / "src/cpu/o3/rob.hh").read_text()
    rob_cc = (ROOT / "src/cpu/o3/rob.cc").read_text()
    commit_cc = (ROOT / "src/cpu/o3/commit.cc").read_text()
    comm_hh = (ROOT / "src/cpu/o3/comm.hh").read_text()
    types = rob_hh[rob_hh.index("    enum class HybridGroupType"):
                   rob_hh.index("  private:",
                                rob_hh.index("    enum class HybridGroupType"))]
    names = ["classifyHybridInst", "planHybridBatch", "insertHybridBatch",
             "insertInstWithGroup", "assertHybridInvariants", "canAllocate",
             "totalEntries", "commitGroup", "squashGroup", "retireHead",
             "drainSquashedHead", "isHeadGroupReady", "doSquash",
             "updateHead", "updateTail"]
    methods = [method(rob_cc, "ROB", name) for name in names]
    declarations = []
    for body in methods:
        signature = body[:body.index("\n{")]
        declarations.append(signature.replace("ROB::", "") + ";")
    arbiter_start = comm_hh.index("struct SmtActiveThreadFreeze")
    arbiter_end = comm_hh.index("struct StallSignals", arbiter_start)
    transition = (method(rob_cc, "ROB", "appendHybridClass")
                  if "ROB::appendHybridClass(" in rob_cc else "")
    return (PREAMBLE + types + "\n".join(declarations) + "\n};\n" +
            comm_hh[arbiter_start:arbiter_end] + COMMIT_FIXTURE +
            transition + "\n".join(methods) + "\n" +
            method(commit_cc, "Commit", "moveInstsToBuffer") + CASES)


class HybridProductionMethods(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tempdir = tempfile.TemporaryDirectory(prefix="hybrid-methods-")
        cls.addClassCleanup(cls.tempdir.cleanup)
        source = Path(cls.tempdir.name) / "hybrid_methods.cc"
        source.write_text(harness())
        cls.binary = source.with_suffix("")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O1",
                        "-D_GLIBCXX_ASSERTIONS", str(source),
                        "-o", str(cls.binary)], check=True)

    def run_case(self, name):
        subprocess.run([str(self.binary), name], check=True)

    def test_one_free_entry(self):
        self.run_case("one_free_entry")

    def test_failed_admission_and_replan(self):
        self.run_case("failed_admission_and_replan")

    def test_squash_window_no_refill(self):
        self.run_case("squash_window_no_refill")

    def test_mixed_squash_window_no_refill(self):
        self.run_case("mixed_squash_window_no_refill")

    def test_partial_retire_and_drain(self):
        self.run_case("partial_retire_and_drain")

    def test_partial_tail_squash(self):
        self.run_case("partial_tail_squash")

    def test_classification(self):
        self.run_case("classification")


if __name__ == "__main__":
    unittest.main(verbosity=2)
