// Unit tests for ChampSimTraceReader buffering, address mapping, and
// branch target lookahead behavior.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "cpu/o3/trace/ChampSimTraceReader.hh"
#include "ext/iostream3/zfstream.h"
#include "gtest/gtest.h"

namespace {

// Recreate the ChampSim binary record layout used by the reader.
// This must match the struct defined in ChampSimTraceReader.
struct CSInstr
{
    uint64_t ip;
    uint8_t is_branch;
    uint8_t branch_taken;
    static constexpr size_t NUM_INSTR_DESTINATIONS = 2;
    static constexpr size_t NUM_INSTR_SOURCES = 4;
    uint8_t destination_registers[NUM_INSTR_DESTINATIONS]{};
    uint8_t source_registers[NUM_INSTR_SOURCES]{};
    uint64_t destination_memory[NUM_INSTR_DESTINATIONS]{};
    uint64_t source_memory[NUM_INSTR_SOURCES]{};
};

static CSInstr
makeInstr(uint64_t ip, bool is_branch = false, bool branch_taken = false,
          std::initializer_list<uint64_t> src_mem = {},
          std::initializer_list<uint64_t> dst_mem = {},
          std::initializer_list<uint8_t> src_regs = {},
          std::initializer_list<uint8_t> dst_regs = {})
{
    CSInstr inst{};
    inst.ip = ip;
    inst.is_branch = is_branch ? 1 : 0;
    inst.branch_taken = branch_taken ? 1 : 0;

    size_t idx = 0;
    for (auto addr : dst_mem) {
        if (idx < CSInstr::NUM_INSTR_DESTINATIONS)
            inst.destination_memory[idx++] = addr;
    }

    idx = 0;
    for (auto addr : src_mem) {
        if (idx < CSInstr::NUM_INSTR_SOURCES)
            inst.source_memory[idx++] = addr;
    }

    idx = 0;
    for (auto reg : src_regs) {
        if (idx < CSInstr::NUM_INSTR_SOURCES)
            inst.source_registers[idx++] = reg;
    }

    idx = 0;
    for (auto reg : dst_regs) {
        if (idx < CSInstr::NUM_INSTR_DESTINATIONS)
            inst.destination_registers[idx++] = reg;
    }

    return inst;
}

enum class TraceFileMode
{
    Binary,
    Gzip,
};

static bool
writeTraceFile(const std::string &path, const std::vector<CSInstr> &insts,
               TraceFileMode mode = TraceFileMode::Binary)
{
    if (mode == TraceFileMode::Binary) {
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs.is_open()) return false;
        for (const auto &ins : insts) {
            ofs.write(reinterpret_cast<const char*>(&ins), sizeof(CSInstr));
            if (!ofs.good()) return false;
        }
        ofs.close();
        return ofs.good();
    }

    gzofstream gz(path.c_str());
    if (!gz.good()) return false;
    for (const auto &ins : insts) {
        gz.write(reinterpret_cast<const char*>(&ins), sizeof(CSInstr));
        if (!gz.good()) return false;
    }
    gz.close();
    return gz.good();
}

// Replicate the reader's hash mapping to validate expectations.
static inline uint64_t
mapHash(uint64_t trace_addr,
        uint64_t base = 0x10000000ULL,
        uint64_t size = 0x40000000ULL,
        bool page_align = true)
{
    constexpr uint64_t PAGE_SIZE = 4096ULL;
    uint64_t hash = (trace_addr ^ (trace_addr >> 16)) & 0x3FFFFFFFULL;
    uint64_t mapped = base + (hash % size);
    if (page_align) {
        mapped = (mapped / PAGE_SIZE) * PAGE_SIZE + (trace_addr % PAGE_SIZE);
        if ((mapped - base) >= size) {
            mapped = base + (trace_addr % size);
        }
    }
    return mapped;
}

static inline uint64_t
mapLinear(uint64_t trace_addr, uint64_t base, uint64_t size, bool page_align)
{
    constexpr uint64_t PAGE_SIZE = 4096ULL;
    uint64_t mapped;
    if (page_align) {
        uint64_t page_offset = trace_addr % PAGE_SIZE;
        uint64_t trace_page = trace_addr / PAGE_SIZE;
        uint64_t mapped_page = trace_page % (size / PAGE_SIZE);
        mapped = base + mapped_page * PAGE_SIZE + page_offset;
    } else {
        mapped = base + (trace_addr % size);
    }
    return mapped;
}

static inline void
markAsCondBranch(CSInstr &inst, bool taken)
{
    // Match ChampSimTraceReader::determineInstType conditional branch pattern:
    // - writes IP
    // - reads IP and FLAGS
    inst.is_branch = 1;
    inst.branch_taken = taken ? 1 : 0;
    inst.destination_registers[0] = 26; // REG_IP
    inst.source_registers[0] = 26;      // REG_IP
    inst.source_registers[1] = 25;      // REG_FL
}

class TraceFileGuard
{
  public:
    explicit TraceFileGuard(std::string path) : tracePath(std::move(path)) {}
    ~TraceFileGuard() { std::remove(tracePath.c_str()); }
    const std::string& path() const { return tracePath; }

  private:
    std::string tracePath;
};

} // anonymous namespace

using gem5::o3::ChampSimTraceReader;
using gem5::o3::TraceInstruction;

TEST(ChampSimTraceReaderTest, ReadsTwoInstructionsAndSetsBranchTargetFromLookahead)
{
    const std::string path = "champsim_reader_test_2.bin";
    TraceFileGuard guard(path);

    CSInstr a{};
    a.ip = 0x1000;
    markAsCondBranch(a, /*taken*/true);
    CSInstr b{};
    b.ip = 0x2000;

    ASSERT_TRUE(writeTraceFile(path, {a, b}));

    ChampSimTraceReader reader(path, "unit.reader");
    ASSERT_TRUE(reader.init());

    // First instruction should be mapped and have branch target set to the
    // mapped PC of the second instruction due to lookahead.
    TraceInstruction i0 = reader.getNextInstruction();
    ASSERT_TRUE(i0.isValid());
    EXPECT_EQ(i0.getPC(), mapHash(a.ip));
    EXPECT_TRUE(i0.getBranch());
    EXPECT_TRUE(i0.getBranchTaken());
    EXPECT_TRUE(i0.getHasBranchTarget());
    EXPECT_EQ(i0.getBranchTarget(), mapHash(b.ip));

    // Second instruction should be valid and mapped.
    TraceInstruction i1 = reader.getNextInstruction();
    ASSERT_TRUE(i1.isValid());
    EXPECT_EQ(i1.getPC(), mapHash(b.ip));
    EXPECT_FALSE(i1.getBranch());

    // Next read should eventually yield invalid (EOF) once buffer drains.
    // Drain any remaining buffered entries first (none expected here), then EOF.
    TraceInstruction i2 = reader.getNextInstruction();
    // Depending on buffering, this may already be invalid.
    if (i2.isValid()) {
        TraceInstruction i3 = reader.getNextInstruction();
        EXPECT_FALSE(i3.isValid());
    } else {
        EXPECT_FALSE(i2.isValid());
    }
}

TEST(ChampSimTraceReaderTest, HashMappingProducesAlignedInRegion)
{
    const std::string path = "champsim_reader_test_hash.bin";
    TraceFileGuard guard(path);
    CSInstr x{}; x.ip = 0xDEADBEEFCAFEBABEULL; x.is_branch = 0; x.branch_taken = 0;
    ASSERT_TRUE(writeTraceFile(path, {x}));

    ChampSimTraceReader reader(path, "unit.reader.hash");
    ASSERT_TRUE(reader.init());

    auto instr = reader.getNextInstruction();
    ASSERT_TRUE(instr.isValid());
    uint64_t pc = instr.getPC();

    // Defaults: base=0x10000000, size=0x40000000
    EXPECT_GE(pc, 0x10000000ULL);
    EXPECT_LT(pc, 0x10000000ULL + 0x40000000ULL);
    EXPECT_EQ(pc, mapHash(x.ip));
}

TEST(ChampSimTraceReaderTest, TakenBranchWithoutLookaheadHasNoTarget)
{
    const std::string path = "champsim_reader_test_branch_eof.bin";
    TraceFileGuard guard(path);
    CSInstr branch = makeInstr(0x4000, /*is_branch*/true, /*taken*/true);
    markAsCondBranch(branch, /*taken*/true);
    ASSERT_TRUE(writeTraceFile(path, {branch}));

    ChampSimTraceReader reader(path, "unit.reader.branch_eof");
    ASSERT_TRUE(reader.init());

    auto instr = reader.getNextInstruction();
    ASSERT_TRUE(instr.isValid());
    EXPECT_TRUE(instr.getBranch());
    EXPECT_TRUE(instr.getBranchTaken());
    EXPECT_FALSE(instr.getHasBranchTarget());
}

TEST(ChampSimTraceReaderTest, NotTakenBranchHasNoTarget)
{
    const std::string path = "champsim_reader_test_branch_not_taken.bin";
    TraceFileGuard guard(path);
    CSInstr branch = makeInstr(0x8000, true, false);
    markAsCondBranch(branch, /*taken*/false);
    CSInstr next = makeInstr(0x8040, false, false);
    ASSERT_TRUE(writeTraceFile(path, {branch, next}));

    ChampSimTraceReader reader(path, "unit.reader.branch_nt");
    ASSERT_TRUE(reader.init());

    auto instr = reader.getNextInstruction();
    ASSERT_TRUE(instr.isValid());
    EXPECT_TRUE(instr.getBranch());
    EXPECT_FALSE(instr.getBranchTaken());
    EXPECT_FALSE(instr.getHasBranchTarget());
}

TEST(ChampSimTraceReaderTest, ExtractsMemoryAndRegisterState)
{
    const std::string path = "champsim_reader_test_memregs.bin";
    TraceFileGuard guard(path);
    CSInstr mem = makeInstr(0x9000, false, false,
                             /*src_mem*/{0xABCDEF100ULL, 0},
                             /*dst_mem*/{0xABCDEF200ULL},
                             /*src_regs*/{5, 7, 9},
                             /*dst_regs*/{10});
    ASSERT_TRUE(writeTraceFile(path, {mem}));

    ChampSimTraceReader reader(path, "unit.reader.memregs");
    ASSERT_TRUE(reader.init());

    auto instr = reader.getNextInstruction();
    ASSERT_TRUE(instr.isValid());

    auto loads = instr.getLoadAddresses();
    ASSERT_EQ(loads.size(), 1U);
    EXPECT_EQ(loads[0], mapHash(0xABCDEF100ULL));

    auto stores = instr.getStoreAddresses();
    ASSERT_EQ(stores.size(), 1U);
    EXPECT_EQ(stores[0], mapHash(0xABCDEF200ULL));

    auto load_vals = instr.getLoadValues();
    ASSERT_EQ(load_vals.size(), 1U);
    EXPECT_EQ(load_vals[0], 0xABCDEF100ULL ^ 0xDEADBEEF);

    auto store_vals = instr.getStoreValues();
    ASSERT_EQ(store_vals.size(), 1U);
    EXPECT_EQ(store_vals[0], (0xABCDEF200ULL ^ 0x9000ULL) + 0x12345678ULL);

    auto src_regs = instr.getSrcRegs();
    // 5 is mapped to x3 by the reader to avoid overloading RA semantics.
    EXPECT_EQ(std::vector<uint8_t>({3, 7, 9}), src_regs);
    auto dst_regs = instr.getDstRegs();
    EXPECT_EQ(std::vector<uint8_t>({10}), dst_regs);
}

TEST(ChampSimTraceReaderTest, NormalizesRegistersToRiscVAbi)
{
    const std::string path = "champsim_reader_test_regmap.bin";
    TraceFileGuard guard(path);
    // src: SP(6)->x2, FLAGS(25)->x0, IP(26)->x0, reg1->x3 (avoid RA), reg5->x3 (avoid alt RA), >32 -> x0
    // dst: SP(6)->x2, FLAGS(25)->x0
    CSInstr rmap = makeInstr(0x7000, false, false,
                             /*src_mem*/{},
                             /*dst_mem*/{},
                             /*src_regs*/{6, 25, 26, 1, 5},
                             /*dst_regs*/{6, 25});
    ASSERT_TRUE(writeTraceFile(path, {rmap}));

    ChampSimTraceReader reader(path, "unit.reader.regmap");
    ASSERT_TRUE(reader.init());

    auto instr = reader.getNextInstruction();
    ASSERT_TRUE(instr.isValid());

    // Current implementation exposes up to four ChampSim source registers;
    // FLAGS/IP are normalized to x0, SP to x2, and RA to x3.
    EXPECT_EQ(instr.getSrcRegs(), (std::vector<uint8_t>{2, 0, 0, 3}));
    EXPECT_EQ(instr.getDstRegs(), (std::vector<uint8_t>{2, 0}));
}

TEST(ChampSimTraceReaderTest, HashMappingWrapsWithinConfiguredWindow)
{
    const std::string path = "champsim_reader_test_hash_wrap.bin";
    TraceFileGuard guard(path);
    CSInstr inst = makeInstr(0x100000000ULL);
    ASSERT_TRUE(writeTraceFile(path, {inst}));

    ChampSimTraceReader reader(path, "unit.reader.hash_wrap");
    reader.setAddressMapping(/*base*/0x30000000ULL, /*size*/0x1800ULL,
                             "hash", /*pageAlign*/true);
    ASSERT_TRUE(reader.init());

    auto instr = reader.getNextInstruction();
    ASSERT_TRUE(instr.isValid());

    uint64_t expected = mapHash(0x100000000ULL, 0x30000000ULL, 0x1800ULL, true);
    EXPECT_EQ(instr.getPC(), expected);
}

TEST(ChampSimTraceReaderTest, HashMappingWithoutPageAlignStillAlignsToWord)
{
    const std::string path = "champsim_reader_test_hash_no_align.bin";
    TraceFileGuard guard(path);
    CSInstr inst = makeInstr(0x12345ULL);
    ASSERT_TRUE(writeTraceFile(path, {inst}));

    ChampSimTraceReader reader(path, "unit.reader.hash_noalign");
    reader.setAddressMapping(/*base*/0x28000000ULL, /*size*/0x2000ULL,
                             "hash", /*pageAlign*/false);
    ASSERT_TRUE(reader.init());

    auto ti = reader.getNextInstruction();
    ASSERT_TRUE(ti.isValid());

    uint64_t expected = mapHash(0x12345ULL, 0x28000000ULL, 0x2000ULL, false);
    EXPECT_EQ(ti.getPC(), expected);
}

TEST(ChampSimTraceReaderTest, LinearMappingHandlesBothAlignmentModes)
{
    const std::string path = "champsim_reader_test_linear_modes.bin";
    TraceFileGuard guard(path);
    CSInstr inst_a = makeInstr(0xABCDEF123ULL);
    CSInstr inst_b = makeInstr(0xABCDEF127ULL);
    ASSERT_TRUE(writeTraceFile(path, {inst_a, inst_b}));

    ChampSimTraceReader reader_aligned(path, "unit.reader.linear_aligned");
    reader_aligned.setAddressMapping(/*base*/0x40000000ULL, /*size*/0x4000ULL,
                                     "linear", /*pageAlign*/true);
    ASSERT_TRUE(reader_aligned.init());
    auto ti_aligned = reader_aligned.getNextInstruction();
    ASSERT_TRUE(ti_aligned.isValid());
    EXPECT_EQ(ti_aligned.getPC(),
              mapLinear(0xABCDEF123ULL, 0x40000000ULL, 0x4000ULL, true));

    ChampSimTraceReader reader_unaligned(path, "unit.reader.linear_unaligned");
    reader_unaligned.setAddressMapping(/*base*/0x50000000ULL, /*size*/0x4000ULL,
                                       "linear", /*pageAlign*/false);
    ASSERT_TRUE(reader_unaligned.init());
    auto ti_unaligned = reader_unaligned.getNextInstruction();
    ASSERT_TRUE(ti_unaligned.isValid());
    EXPECT_EQ(ti_unaligned.getPC(),
              mapLinear(0xABCDEF123ULL, 0x50000000ULL, 0x4000ULL, false));
}

TEST(ChampSimTraceReaderTest, ResetReturnsToBeginning)
{
    const std::string path = "champsim_reader_test_reset.bin";
    TraceFileGuard guard(path);
    CSInstr first = makeInstr(0x1000);
    CSInstr second = makeInstr(0x2000);
    ASSERT_TRUE(writeTraceFile(path, {first, second}));

    ChampSimTraceReader reader(path, "unit.reader.reset");
    ASSERT_TRUE(reader.init());

    auto i0 = reader.getNextInstruction();
    auto i1 = reader.getNextInstruction();
    ASSERT_TRUE(i0.isValid());
    ASSERT_TRUE(i1.isValid());
    EXPECT_NE(i0.getPC(), i1.getPC());

    ASSERT_TRUE(reader.reset());
    auto again = reader.getNextInstruction();
    ASSERT_TRUE(again.isValid());
    EXPECT_EQ(again.getPC(), i0.getPC());
}

TEST(ChampSimTraceReaderTest, RestoreCheckpointRecoversBufferedState)
{
    const std::string path = "champsim_reader_test_checkpoint.bin";
    TraceFileGuard guard(path);
    std::vector<CSInstr> insts;
    const int total_insts = 1024;
    insts.reserve(total_insts);
    for (int i = 0; i < total_insts; ++i) {
        insts.push_back(makeInstr(0x1000 + i * 0x10));
    }
    ASSERT_TRUE(writeTraceFile(path, insts));

    ChampSimTraceReader reader(path, "unit.reader.checkpoint");
    ASSERT_TRUE(reader.init());

    auto first = reader.getNextInstruction();
    auto second = reader.getNextInstruction();
    ASSERT_TRUE(first.isValid());
    ASSERT_TRUE(second.isValid());

    auto checkpoint = reader.createCheckpoint();
    ASSERT_TRUE(checkpoint.valid);
    auto third = reader.getNextInstruction();
    ASSERT_TRUE(third.isValid());

    ASSERT_TRUE(reader.restoreCheckpoint(checkpoint));
    auto replay = reader.getNextInstruction();
    ASSERT_TRUE(replay.isValid());
    EXPECT_EQ(replay.getPC(), third.getPC());
}

TEST(ChampSimTraceReaderTest, SeekToInstructionWithoutCheckpointsResetsStream)
{
    const std::string path = "champsim_reader_test_seek.bin";
    TraceFileGuard guard(path);
    std::vector<CSInstr> insts;
    for (int i = 0; i < 4; ++i) {
        insts.push_back(makeInstr(0x5000 + i * 0x10));
    }
    ASSERT_TRUE(writeTraceFile(path, insts));

    ChampSimTraceReader reader(path, "unit.reader.seek");
    ASSERT_TRUE(reader.init());

    ASSERT_TRUE(reader.seekToInstruction(2));
    auto instr = reader.getNextInstruction();
    ASSERT_TRUE(instr.isValid());
    EXPECT_EQ(instr.getPC(), mapHash(insts[2].ip));
}

TEST(ChampSimTraceReaderTest, HandlesCompressedTraceInput)
{
    const std::string path = "champsim_reader_test_compressed.bin.gz";
    TraceFileGuard guard(path);
    std::vector<CSInstr> insts;
    for (int i = 0; i < 64; ++i) {
        insts.push_back(makeInstr(0x12345000 + i * 0x10));
    }
    ASSERT_TRUE(writeTraceFile(path, insts, TraceFileMode::Gzip));

    ChampSimTraceReader reader(path, "unit.reader.gz");
    ASSERT_TRUE(reader.init());

    auto ti = reader.getNextInstruction();
    ASSERT_TRUE(ti.isValid());
    EXPECT_EQ(ti.getPC(), mapHash(0x12345000ULL));
}

TEST(ChampSimTraceReaderTest, InitFailsForTooSmallTrace)
{
    const std::string path = "champsim_reader_test_small.bin";
    TraceFileGuard guard(path);
    std::ofstream ofs(path, std::ios::binary);
    ASSERT_TRUE(ofs.is_open());
    uint32_t dummy = 0xDEADBEEF;
    ofs.write(reinterpret_cast<const char*>(&dummy), sizeof(dummy));
    ofs.close();

    ChampSimTraceReader reader(path, "unit.reader.small");
    EXPECT_FALSE(reader.init());
}

TEST(ChampSimTraceReaderTest, LinearMappingPageAlignedPreservesOffset)
{
    const std::string path = "champsim_reader_test_linear.bin";
    TraceFileGuard guard(path);
    CSInstr x{}; x.ip = 0x12345678ABCDEF10ULL; x.is_branch = 0; x.branch_taken = 0;
    ASSERT_TRUE(writeTraceFile(path, {x}));

    ChampSimTraceReader reader(path, "unit.reader.linear");
    // Configure linear mapping with a small window and page alignment
    reader.setAddressMapping(/*base*/0x20000000ULL, /*size*/0x10000ULL,
                             /*mode*/"linear", /*pageAlign*/true);
    ASSERT_TRUE(reader.init());

    auto instr = reader.getNextInstruction();
    ASSERT_TRUE(instr.isValid());
    uint64_t pc = instr.getPC();

    EXPECT_GE(pc, 0x20000000ULL);
    EXPECT_LT(pc, 0x20000000ULL + 0x10000ULL);
    EXPECT_EQ(pc & 0x3ULL, 0ULL);
    // Page-offset preserved
    constexpr uint64_t PAGE_SIZE = 4096ULL;
    EXPECT_EQ(pc % PAGE_SIZE, x.ip % PAGE_SIZE);
}
