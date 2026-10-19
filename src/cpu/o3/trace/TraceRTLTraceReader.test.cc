// Unit tests for TraceRTLTraceReader (TRACERTL/NEMU 48-byte format):
// field mapping (all 8 branch types, all 4 memory size codes, exception
// markers), raw-encoding publication/suppression rules, zstd/raw stream
// parity, checkpoint/restore and seek semantics.

#include <zstd.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "cpu/o3/trace/TraceRTLTraceReader.hh"
#include "gtest/gtest.h"

namespace {

// Recreate the TRACERTL binary record layout used by the reader (must match
// the struct in TraceRTLTraceReader.hh: 48 bytes, little-endian).
struct RTLInstr
{
    uint64_t instr_pc_va;
    uint64_t instr_pc_pa;
    uint64_t mem_va;
    uint64_t mem_pa;
    uint64_t target;
    uint32_t instr;
    uint8_t  mem_type_size;
    uint8_t  branch_type;
    uint8_t  branch_taken;
    uint8_t  exception;
};
static_assert(sizeof(RTLInstr) == 48, "fixture record must be 48 bytes");

// Realistic RV64 encodings (from the gwt-v2 corpus where possible).
constexpr uint32_t ENC_ADDI   = 0x00000013; // addi x0, x0, 0
constexpr uint32_t ENC_BEQ    = 0x00088063; // beq  x17, x0, 0 (imm=0!)
constexpr uint32_t ENC_LW     = 0x00032283; // lw   x5, 0(x6)
constexpr uint32_t ENC_SW     = 0x00632023; // sw   x5, 0(x6)
constexpr uint32_t ENC_JAL    = 0x0000006F; // jal  x0, 0
constexpr uint32_t ENC_JALR   = 0x00008067; // jalr x0, 0(x1)
constexpr uint32_t ENC_FADD   = 0x02000053; // fadd.d (opcode 0x53)
constexpr uint32_t ENC_NOP33  = 0x00000033; // corpus NOP placeholder
constexpr uint32_t ENC_RVC    = 0x00000001; // c.nop (16-bit)

static RTLInstr
makeRec(uint64_t pc, uint32_t instr = ENC_ADDI, uint8_t mem_type = 0,
        uint8_t size_code = 0, uint64_t mem_va = 0, uint8_t branch_type = 0,
        uint8_t branch_taken = 0, uint8_t exception = 0, uint64_t target = 0)
{
    RTLInstr r{};
    r.instr_pc_va = pc;
    r.instr_pc_pa = 0;            // corpus: always 0, never consumed
    r.mem_va = mem_va;
    r.mem_pa = 0xDEADBEEFCAFEBABEULL; // corpus: stale garbage, never consumed
    r.target = target;
    r.instr = instr;
    r.mem_type_size = (mem_type & 0xF) | (static_cast<uint8_t>(size_code << 4));
    r.branch_type = branch_type;
    r.branch_taken = branch_taken;
    r.exception = exception;
    return r;
}

static bool
writeRawTraceFile(const std::string &path, const std::vector<RTLInstr> &recs)
{
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    for (const auto &r : recs) {
        ofs.write(reinterpret_cast<const char*>(&r), sizeof(RTLInstr));
        if (!ofs.good()) return false;
    }
    ofs.close();
    return ofs.good();
}

static bool
writeZstdTraceFile(const std::string &path, const std::vector<RTLInstr> &recs)
{
    std::vector<char> raw;
    raw.reserve(recs.size() * sizeof(RTLInstr));
    for (const auto &r : recs) {
        const char *p = reinterpret_cast<const char*>(&r);
        raw.insert(raw.end(), p, p + sizeof(RTLInstr));
    }
    const size_t bound = ZSTD_compressBound(raw.size());
    std::vector<char> compressed(bound);
    const size_t csize = ZSTD_compress(compressed.data(), compressed.size(),
                                       raw.data(), raw.size(), /*level*/3);
    if (ZSTD_isError(csize)) return false;
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    ofs.write(compressed.data(), csize);
    ofs.close();
    return ofs.good();
}

/** Two independently compressed frames concatenated in one file (streamed
 *  frame concatenation): the decompressor must serve them back-to-back
 *  with no gaps or duplicates. */
static bool
writeZstdTwoFrameTraceFile(const std::string &path,
                           const std::vector<RTLInstr> &recs)
{
    const size_t half = recs.size() / 2;
    auto compressRange = [](const std::vector<RTLInstr> &rs,
                            size_t begin, size_t end) {
        std::vector<char> raw;
        raw.reserve((end - begin) * sizeof(RTLInstr));
        for (size_t i = begin; i < end; ++i) {
            const char *p = reinterpret_cast<const char*>(&rs[i]);
            raw.insert(raw.end(), p, p + sizeof(RTLInstr));
        }
        const size_t bound = ZSTD_compressBound(raw.size());
        std::vector<char> out(bound);
        const size_t csize = ZSTD_compress(out.data(), out.size(),
                                           raw.data(), raw.size(), 3);
        if (ZSTD_isError(csize)) return std::vector<char>();
        out.resize(csize);
        return out;
    };
    const auto frameA = compressRange(recs, 0, half);
    const auto frameB = compressRange(recs, half, recs.size());
    if (frameA.empty() || frameB.empty()) return false;
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    ofs.write(frameA.data(), frameA.size());
    ofs.write(frameB.data(), frameB.size());
    ofs.close();
    return ofs.good();
}

/** A single frame truncated mid-stream (all but the last 16 bytes kept). */
static bool
writeZstdTruncatedTraceFile(const std::string &path,
                            const std::vector<RTLInstr> &recs)
{
    std::vector<char> raw;
    raw.reserve(recs.size() * sizeof(RTLInstr));
    for (const auto &r : recs) {
        const char *p = reinterpret_cast<const char*>(&r);
        raw.insert(raw.end(), p, p + sizeof(RTLInstr));
    }
    const size_t bound = ZSTD_compressBound(raw.size());
    std::vector<char> compressed(bound);
    const size_t csize = ZSTD_compress(compressed.data(), compressed.size(),
                                       raw.data(), raw.size(), 3);
    if (ZSTD_isError(csize) || csize <= 16) return false;
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    ofs.write(compressed.data(), csize - 16);
    ofs.close();
    return ofs.good();
}

// Replicate the reader's hash mapping to validate expectations (same
// helper contract as the ChampSim reader tests).
static inline uint64_t
mapHash(uint64_t trace_addr,
        uint64_t base = 0x10000000ULL,
        uint64_t size = 0x40000000ULL)
{
    constexpr uint64_t PAGE_SIZE = 4096ULL;
    uint64_t hash = (trace_addr ^ (trace_addr >> 16)) & 0x3FFFFFFFULL;
    uint64_t mapped = base + (hash % size);
    // pageAlign=true default
    mapped = (mapped / PAGE_SIZE) * PAGE_SIZE + (trace_addr % PAGE_SIZE);
    if ((mapped - base) >= size) {
        mapped = base + (trace_addr % size);
    }
    return mapped;
}

static inline uint64_t
mapMemHash(uint64_t trace_addr)
{
    return mapHash(trace_addr) & ~0x3ULL;
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

using gem5::o3::TraceInstruction;
using gem5::o3::TraceRTLTraceReader;

TEST(TraceRTLTraceReaderTest, PublishesRawEncodingAndMapsPC)
{
    const std::string path = "tracertl_raw_enc.bin";
    TraceFileGuard guard(path);
    ASSERT_TRUE(writeRawTraceFile(path, {makeRec(0x5606B269457CULL, ENC_BEQ)}));

    TraceRTLTraceReader reader(path, "unit.tracertl.rawenc");
    ASSERT_TRUE(reader.init());
    EXPECT_EQ(reader.getFormat(), "tracertl");
    EXPECT_TRUE(reader.supportsFastRandomSeek());

    auto instr = reader.getNextInstruction();
    ASSERT_TRUE(instr.isValid());
    EXPECT_EQ(instr.getPC(), mapHash(0x5606B269457CULL));
    EXPECT_TRUE(instr.hasRawInstBits());
    EXPECT_EQ(instr.getInstBits(), ENC_BEQ);
    EXPECT_EQ(instr.getInstSizeBytes(), 4U);
}

TEST(TraceRTLTraceReaderTest, BranchTypeTableCoversAllKinds)
{
    const std::string path = "tracertl_branch_types.bin";
    TraceFileGuard guard(path);
    // One record per branch_type (1..7) plus a non-branch record.
    const uint64_t base_pc = 0x560600000000ULL;
    const uint64_t tgt = base_pc + 0x1000ULL;
    std::vector<RTLInstr> recs = {
        makeRec(base_pc + 0x00, ENC_BEQ, 0, 0, 0, /*bt*/1, /*taken*/1, 0, tgt),
        makeRec(base_pc + 0x04, ENC_BEQ, 0, 0, 0, /*bt*/2, /*taken*/0, 0, tgt),
        makeRec(base_pc + 0x08, ENC_JAL, 0, 0, 0, /*bt*/3, 1, 0, tgt),
        makeRec(base_pc + 0x0C, ENC_JALR, 0, 0, 0, /*bt*/4, 1, 0, tgt),
        makeRec(base_pc + 0x10, ENC_JAL, 0, 0, 0, /*bt*/5, 1, 0, tgt),
        makeRec(base_pc + 0x14, ENC_JALR, 0, 0, 0, /*bt*/6, 1, 0, tgt),
        makeRec(base_pc + 0x18, ENC_JALR, 0, 0, 0, /*bt*/7, 1, 0, tgt),
        makeRec(base_pc + 0x1C, ENC_ADDI),
    };
    ASSERT_TRUE(writeRawTraceFile(path, recs));

    TraceRTLTraceReader reader(path, "unit.tracertl.bt");
    ASSERT_TRUE(reader.init());

    struct Expected { TraceInstruction::InstType type; bool taken; };
    const Expected table[] = {
        {TraceInstruction::InstType::COND_BRANCH, true},
        {TraceInstruction::InstType::COND_BRANCH, false},
        {TraceInstruction::InstType::UNCOND_DIRECT_BRANCH, true},
        {TraceInstruction::InstType::UNCOND_INDIRECT_BRANCH, true},
        {TraceInstruction::InstType::CALL_DIRECT, true},
        {TraceInstruction::InstType::CALL_INDIRECT, true},
        {TraceInstruction::InstType::RETURN, true},
    };

    for (size_t i = 0; i < 7; ++i) {
        auto instr = reader.getNextInstruction();
        ASSERT_TRUE(instr.isValid()) << "record " << i;
        EXPECT_EQ(instr.getInstType(), table[i].type) << "record " << i;
        EXPECT_TRUE(instr.getBranch()) << "record " << i;
        EXPECT_EQ(instr.getBranchTaken(), table[i].taken) << "record " << i;
        // The target field is authoritative (tracer zeroes the immediates).
        EXPECT_TRUE(instr.getHasBranchTarget()) << "record " << i;
        EXPECT_EQ(instr.getBranchTarget(), mapHash(tgt)) << "record " << i;
        EXPECT_FALSE(instr.isCtrlFlowChange()) << "record " << i;
        // Real encodings are published for branch records.
        EXPECT_TRUE(instr.hasRawInstBits()) << "record " << i;
    }

    auto plain = reader.getNextInstruction();
    ASSERT_TRUE(plain.isValid());
    EXPECT_EQ(plain.getInstType(), TraceInstruction::InstType::ALU);
    EXPECT_FALSE(plain.getBranch());
}

TEST(TraceRTLTraceReaderTest, MemoryTypeAndSizeCodeMapping)
{
    const std::string path = "tracertl_mem_sizes.bin";
    TraceFileGuard guard(path);
    const uint64_t pc = 0x560700000000ULL;
    const uint64_t mem = 0x733DBC1B4020ULL;
    // size code -> bytes: 0=1B, 1=2B, 2=4B, 3=8B (loads then stores)
    std::vector<RTLInstr> recs = {
        makeRec(pc + 0x00, ENC_LW, /*mem_type*/1, /*code*/0, mem),
        makeRec(pc + 0x04, ENC_LW, 1, 1, mem + 4),
        makeRec(pc + 0x08, ENC_LW, 1, 2, mem + 8),
        makeRec(pc + 0x0C, ENC_LW, 1, 3, mem + 16),
        makeRec(pc + 0x10, ENC_SW, /*mem_type*/2, 0, mem + 32),
        makeRec(pc + 0x14, ENC_SW, 2, 1, mem + 36),
        makeRec(pc + 0x18, ENC_SW, 2, 2, mem + 40),
        makeRec(pc + 0x1C, ENC_SW, 2, 3, mem + 48),
    };
    ASSERT_TRUE(writeRawTraceFile(path, recs));

    TraceRTLTraceReader reader(path, "unit.tracertl.mem");
    ASSERT_TRUE(reader.init());

    const uint32_t want_bytes[4] = {1, 2, 4, 8};
    // load mem_va offsets: +0, +4, +8, +16; store mem_va offsets: +32,
    // +36, +40, +48 (mirroring the fixture above).
    const uint64_t load_off[4] = {0, 4, 8, 16};
    const uint64_t store_off[4] = {32, 36, 40, 48};
    for (int i = 0; i < 4; ++i) {
        auto load = reader.getNextInstruction();
        ASSERT_TRUE(load.isValid()) << "load " << i;
        EXPECT_EQ(load.getInstType(), TraceInstruction::InstType::LOAD)
            << "load " << i;
        ASSERT_EQ(load.getLoadAddresses().size(), 1U) << "load " << i;
        EXPECT_EQ(load.getLoadAddresses()[0], mapMemHash(mem + load_off[i]))
            << "load " << i;
        ASSERT_EQ(load.getMemSizes().size(), 1U) << "load " << i;
        EXPECT_EQ(load.getMemSizes()[0], want_bytes[i]) << "load " << i;
        ASSERT_EQ(load.getLoadValues().size(), 1U) << "load " << i;
        EXPECT_EQ(load.getLoadValues()[0], (mem + load_off[i]) ^ 0xDEADBEEFULL)
            << "load " << i;
    }
    for (int i = 0; i < 4; ++i) {
        auto store = reader.getNextInstruction();
        ASSERT_TRUE(store.isValid()) << "store " << i;
        EXPECT_EQ(store.getInstType(), TraceInstruction::InstType::STORE)
            << "store " << i;
        ASSERT_EQ(store.getStoreAddresses().size(), 1U) << "store " << i;
        EXPECT_EQ(store.getStoreAddresses()[0],
                  mapMemHash(mem + store_off[i])) << "store " << i;
        ASSERT_EQ(store.getMemSizes().size(), 1U) << "store " << i;
        EXPECT_EQ(store.getMemSizes()[0], want_bytes[i]) << "store " << i;
    }
}

TEST(TraceRTLTraceReaderTest, FpOpcodeClassifiesAsFp)
{
    const std::string path = "tracertl_fp.bin";
    TraceFileGuard guard(path);
    ASSERT_TRUE(writeRawTraceFile(path, {makeRec(0x560800000000ULL, ENC_FADD)}));

    TraceRTLTraceReader reader(path, "unit.tracertl.fp");
    ASSERT_TRUE(reader.init());

    auto instr = reader.getNextInstruction();
    ASSERT_TRUE(instr.isValid());
    EXPECT_EQ(instr.getInstType(), TraceInstruction::InstType::FP);
    EXPECT_TRUE(instr.hasRawInstBits());
}

TEST(TraceRTLTraceReaderTest, ExceptionMarkerSetsCtrlFlowChange)
{
    // pc_discontinuity marker: NOP placeholder encoding + forced jump to
    // target. The raw bits are kept (NOP classifies as ALU, matching the
    // trace-side type), and the redirect rides the ctrl-flow channel.
    const std::string path = "tracertl_exc_nop.bin";
    TraceFileGuard guard(path);
    const uint64_t pc = 0x7F09F8328E84ULL;
    const uint64_t tgt = 0x5606B26948B0ULL;
    ASSERT_TRUE(writeRawTraceFile(path,
        {makeRec(pc, ENC_NOP33, 0, 0, 0, 0, 0, /*exception*/1, tgt),
         makeRec(tgt, ENC_ADDI)}));

    TraceRTLTraceReader reader(path, "unit.tracertl.exc");
    ASSERT_TRUE(reader.init());

    auto marker = reader.getNextInstruction();
    ASSERT_TRUE(marker.isValid());
    EXPECT_EQ(marker.getInstType(), TraceInstruction::InstType::ALU);
    EXPECT_TRUE(marker.isCtrlFlowChange());
    EXPECT_TRUE(marker.getHasCtrlFlowTarget());
    EXPECT_EQ(marker.getCtrlFlowTarget(), mapHash(tgt));
    // A NOP encoding is safe to publish: it classifies as ALU on both
    // sides of the difftest.
    EXPECT_TRUE(marker.hasRawInstBits());
    EXPECT_EQ(marker.getInstBits(), ENC_NOP33);

    auto at_target = reader.getNextInstruction();
    ASSERT_TRUE(at_target.isValid());
    EXPECT_EQ(at_target.getPC(), mapHash(tgt));
}

TEST(TraceRTLTraceReaderTest, ExceptionMarkerWithBranchEncodingSanitizedToNop)
{
    // The corpus occasionally records the real static encoding of the
    // marker PC (a beq) while the metadata still says non-branch. The
    // published encoding must be sanitized to a NOP: the decoded type
    // stays ALU (matching the trace side), and the synthetic path is
    // bypassed entirely. The PC redirect still rides the ctrl-flow
    // channel.
    const std::string path = "tracertl_exc_branch.bin";
    TraceFileGuard guard(path);
    const uint64_t pc = 0x5606B26945D0ULL;
    const uint64_t tgt = 0x5606B259B3E0ULL;
    ASSERT_TRUE(writeRawTraceFile(path,
        {makeRec(pc, ENC_BEQ, 0, 0, 0, 0, 0, /*exception*/1, tgt),
         makeRec(tgt, ENC_ADDI)}));

    TraceRTLTraceReader reader(path, "unit.tracertl.excbranch");
    ASSERT_TRUE(reader.init());

    auto marker = reader.getNextInstruction();
    ASSERT_TRUE(marker.isValid());
    EXPECT_EQ(marker.getInstType(), TraceInstruction::InstType::ALU);
    EXPECT_TRUE(marker.hasRawInstBits())
        << "sanitized NOP is published as the raw encoding";
    EXPECT_EQ(marker.getInstBits(), 0x00000013U)
        << "branch-opcode encodings on exception markers become NOP";
    EXPECT_TRUE(marker.isCtrlFlowChange());
    EXPECT_EQ(marker.getCtrlFlowTarget(), mapHash(tgt));
}

TEST(TraceRTLTraceReaderTest, RvcEncodingFallsBackToSynthetic)
{
    const std::string path = "tracertl_rvc.bin";
    TraceFileGuard guard(path);
    ASSERT_TRUE(writeRawTraceFile(path, {makeRec(0x560900000000ULL, ENC_RVC)}));

    TraceRTLTraceReader reader(path, "unit.tracertl.rvc");
    ASSERT_TRUE(reader.init());

    auto instr = reader.getNextInstruction();
    ASSERT_TRUE(instr.isValid());
    EXPECT_FALSE(instr.hasRawInstBits())
        << "v1 supports 32-bit encodings only; RVC falls back to synthetic";
    EXPECT_EQ(instr.getInstType(), TraceInstruction::InstType::ALU);
}

TEST(TraceRTLTraceReaderTest, ZstdAndRawStreamsYieldIdenticalInstructions)
{
    // Same records through both containers: the instruction stream must be
    // identical record-by-record (raw-vs-zstd parity).
    std::vector<RTLInstr> recs;
    const uint64_t base = 0x560A00000000ULL;
    for (int i = 0; i < 64; ++i) {
        recs.push_back(makeRec(base + i * 4, ENC_ADDI));
    }
    recs.push_back(makeRec(base + 0x1000, ENC_BEQ, 0, 0, 0, /*bt*/1, 1, 0,
                           base + 0x2000));
    recs.push_back(makeRec(base + 0x2000, ENC_LW, 1, 2, 0x733D00000000ULL));
    recs.push_back(makeRec(base + 0x2004, ENC_NOP33, 0, 0, 0, 0, 0, 1,
                           base + 0x3000));

    const std::string raw_path = "tracertl_parity_raw.bin";
    const std::string zstd_path = "tracertl_parity_raw.bin.zstd";
    TraceFileGuard raw_guard(raw_path), zstd_guard(zstd_path);
    ASSERT_TRUE(writeRawTraceFile(raw_path, recs));
    ASSERT_TRUE(writeZstdTraceFile(zstd_path, recs));

    TraceRTLTraceReader raw_reader(raw_path, "unit.tracertl.parity.raw");
    ASSERT_TRUE(raw_reader.init());
    EXPECT_TRUE(raw_reader.supportsFastRandomSeek());

    TraceRTLTraceReader zstd_reader(zstd_path, "unit.tracertl.parity.zstd");
    ASSERT_TRUE(zstd_reader.init());
    EXPECT_FALSE(zstd_reader.supportsFastRandomSeek())
        << "compressed streams cannot fast-seek";

    int compared = 0;
    for (;;) {
        auto a = raw_reader.getNextInstruction();
        auto b = zstd_reader.getNextInstruction();
        EXPECT_EQ(a.isValid(), b.isValid());
        if (!a.isValid()) break;
        ASSERT_TRUE(b.isValid());
        EXPECT_EQ(a.getPC(), b.getPC()) << "record " << compared;
        EXPECT_EQ(a.getInstType(), b.getInstType()) << "record " << compared;
        EXPECT_EQ(a.getInstBits(), b.getInstBits()) << "record " << compared;
        EXPECT_EQ(a.getBranchTaken(), b.getBranchTaken()) << "record " << compared;
        EXPECT_EQ(a.getBranchTarget(), b.getBranchTarget())
            << "record " << compared;
        EXPECT_EQ(a.isCtrlFlowChange(), b.isCtrlFlowChange())
            << "record " << compared;
        EXPECT_EQ(a.getCtrlFlowTarget(), b.getCtrlFlowTarget())
            << "record " << compared;
        ++compared;
    }
    EXPECT_EQ(compared, static_cast<int>(recs.size()));
    EXPECT_TRUE(raw_reader.isEOF());
    EXPECT_TRUE(zstd_reader.isEOF());
}

TEST(TraceRTLTraceReaderTest, ZstdDetectedByMagicNotSuffix)
{
    // A zstd container under a bare name must still decompress (magic-based
    // detection), and a raw file wearing a .zstd suffix stays raw.
    std::vector<RTLInstr> recs = {makeRec(0x560B00000000ULL, ENC_ADDI),
                                  makeRec(0x560B00000004ULL, ENC_ADDI)};

    const std::string zstd_no_suffix = "tracertl_magic_no_suffix.trace";
    const std::string raw_with_suffix = "tracertl_magic_raw.trace.zstd";
    TraceFileGuard g1(zstd_no_suffix), g2(raw_with_suffix);
    ASSERT_TRUE(writeZstdTraceFile(zstd_no_suffix, recs));
    ASSERT_TRUE(writeRawTraceFile(raw_with_suffix, recs));

    TraceRTLTraceReader a(zstd_no_suffix, "unit.tracertl.magic.zstd");
    ASSERT_TRUE(a.init());
    auto ia = a.getNextInstruction();
    ASSERT_TRUE(ia.isValid());
    EXPECT_EQ(ia.getPC(), mapHash(0x560B00000000ULL));

    TraceRTLTraceReader b(raw_with_suffix, "unit.tracertl.magic.raw");
    ASSERT_TRUE(b.init());
    auto ib = b.getNextInstruction();
    ASSERT_TRUE(ib.isValid());
    EXPECT_EQ(ib.getPC(), mapHash(0x560B00000000ULL));
}

TEST(TraceRTLTraceReaderTest, CheckpointRestoreRawAndZstd)
{
    // Mid-stream checkpoints must restore exactly, on raw (file seek) and
    // on zstd (reopen + fast-forward replay).
    std::vector<RTLInstr> recs;
    const uint64_t base = 0x560C00000000ULL;
    for (int i = 0; i < 2048; ++i) {
        recs.push_back(makeRec(base + i * 4, ENC_ADDI));
    }

    for (const auto container : {"raw", "zstd"}) {
        const std::string path = std::string("tracertl_ckpt.") + container;
        TraceFileGuard guard(path);
        if (std::string(container) == "raw") {
            ASSERT_TRUE(writeRawTraceFile(path, recs));
        } else {
            ASSERT_TRUE(writeZstdTraceFile(path, recs));
        }

        TraceRTLTraceReader reader(path,
                                   std::string("unit.tracertl.ckpt.") + container);
        ASSERT_TRUE(reader.init());

        for (int i = 0; i < 100; ++i)
            reader.getNextInstruction();

        const auto cp = reader.createCheckpoint();
        ASSERT_TRUE(cp.valid) << container;
        const uint64_t idx_at_cp = reader.getCurrentInstructionIndex();

        auto after = reader.getNextInstruction();
        auto after2 = reader.getNextInstruction();
        ASSERT_TRUE(after.isValid());
        ASSERT_TRUE(after2.isValid());

        ASSERT_TRUE(reader.restoreCheckpoint(cp)) << container;
        EXPECT_EQ(reader.getCurrentInstructionIndex(), idx_at_cp)
            << container;
        auto replay = reader.getNextInstruction();
        ASSERT_TRUE(replay.isValid()) << container;
        EXPECT_EQ(replay.getPC(), after.getPC()) << container;
        auto replay2 = reader.getNextInstruction();
        ASSERT_TRUE(replay2.isValid()) << container;
        EXPECT_EQ(replay2.getPC(), after2.getPC()) << container;
    }
}

TEST(TraceRTLTraceReaderTest, SeekAndSoftSeekSemantics)
{
    // Contract shared with the other readers: after seek(N), the next
    // getNextInstruction() returns record N+1 (0-based recs[N]).
    std::vector<RTLInstr> recs;
    const uint64_t base = 0x560D00000000ULL;
    for (int i = 0; i < 8; ++i) {
        recs.push_back(makeRec(base + i * 0x10, ENC_ADDI));
    }

    for (const auto container : {"raw", "zstd"}) {
        const std::string path = std::string("tracertl_seek.") + container;
        TraceFileGuard guard(path);
        if (std::string(container) == "raw") {
            ASSERT_TRUE(writeRawTraceFile(path, recs));
        } else {
            ASSERT_TRUE(writeZstdTraceFile(path, recs));
        }

        TraceRTLTraceReader reader(path,
                                   std::string("unit.tracertl.seek.") + container);
        ASSERT_TRUE(reader.init());

        ASSERT_TRUE(reader.seekToInstruction(3)) << container;
        auto instr = reader.getNextInstruction();
        ASSERT_TRUE(instr.isValid()) << container;
        EXPECT_EQ(instr.getPC(), mapHash(recs[3].instr_pc_va)) << container;

        ASSERT_TRUE(reader.seekToInstruction(0)) << container;
        instr = reader.getNextInstruction();
        ASSERT_TRUE(instr.isValid()) << container;
        EXPECT_EQ(instr.getPC(), mapHash(recs[0].instr_pc_va)) << container;

        // softSeek replay after consuming everything: rollback into the
        // history window must replay in order without duplication.
        ASSERT_TRUE(reader.softSeekToInstruction(1)) << container;
        for (int i = 1; i < 8; ++i) {
            auto replay = reader.getNextInstruction();
            ASSERT_TRUE(replay.isValid()) << container << " i=" << i;
            EXPECT_EQ(replay.getPC(), mapHash(recs[i].instr_pc_va))
                << container << " i=" << i;
        }
        int extra = 0;
        while (reader.getNextInstruction().isValid())
            ++extra;
        EXPECT_EQ(extra, 0) << container << " replay must not duplicate";
        EXPECT_TRUE(reader.isEOF()) << container;
    }
}

TEST(TraceRTLTraceReaderTest, ReadToEofYieldsExactlyNRecords)
{
    const std::string raw_path = "tracertl_eof_raw.bin";
    const std::string zstd_path = "tracertl_eof.zstd";
    TraceFileGuard g1(raw_path), g2(zstd_path);

    std::vector<RTLInstr> recs;
    for (int i = 0; i < 16; ++i) {
        recs.push_back(makeRec(0x560E00000000ULL + i * 4, ENC_ADDI));
    }
    ASSERT_TRUE(writeRawTraceFile(raw_path, recs));
    ASSERT_TRUE(writeZstdTraceFile(zstd_path, recs));

    for (const auto *path : {raw_path.c_str(), zstd_path.c_str()}) {
        TraceRTLTraceReader reader(path, "unit.tracertl.eof");
        ASSERT_TRUE(reader.init());
        int count = 0;
        while (reader.getNextInstruction().isValid())
            ++count;
        EXPECT_EQ(count, 16) << path;
        EXPECT_TRUE(reader.isEOF()) << path;
    }
}

TEST(TraceRTLTraceReaderTest, DualEncodingRecordsRemainReadable)
{
    // R2 artifact: a PC first recorded as a NOP marker and later with its
    // real encoding. Both records must publish their own bits, and the
    // re-encoding must be counted exactly once.
    const std::string path = "tracertl_dual_enc.bin";
    TraceFileGuard guard(path);
    const uint64_t pc = 0x1A79DF34ULL;
    ASSERT_TRUE(writeRawTraceFile(path,
        {makeRec(pc, ENC_NOP33, 0, 0, 0, 0, 0, 1, pc + 0x100),
         makeRec(pc, ENC_BEQ, 0, 0, 0, /*bt*/1, /*taken*/1, 0, pc + 0x200)}));

    TraceRTLTraceReader reader(path, "unit.tracertl.dualenc");
    ASSERT_TRUE(reader.init());

    auto first = reader.getNextInstruction();
    ASSERT_TRUE(first.isValid());
    EXPECT_TRUE(first.hasRawInstBits());
    EXPECT_EQ(first.getInstBits(), ENC_NOP33);
    EXPECT_TRUE(first.isCtrlFlowChange());

    auto second = reader.getNextInstruction();
    ASSERT_TRUE(second.isValid());
    EXPECT_TRUE(second.hasRawInstBits());
    EXPECT_EQ(second.getInstBits(), ENC_BEQ);
    EXPECT_EQ(second.getInstType(), TraceInstruction::InstType::COND_BRANCH);
    EXPECT_EQ(second.getBranchTarget(), mapHash(pc + 0x200));

    // One NOP -> beq transition for the same PC.
    EXPECT_EQ(reader.getMixedEncodingPcCount(), 1U);
}

TEST(TraceRTLTraceReaderTest, ZstdTwoFrameConcatenationDrainsFully)
{
    // Two independently compressed frames in one file: the streaming
    // decompressor must serve them back-to-back (guards zstdRefill's
    // multi-frame continuation).
    std::vector<RTLInstr> recs;
    const uint64_t base = 0x561000000000ULL;
    for (int i = 0; i < 100; ++i) {
        recs.push_back(makeRec(base + i * 4, ENC_ADDI));
    }
    const std::string path = "tracertl_twoframe.bin.zstd";
    TraceFileGuard guard(path);
    ASSERT_TRUE(writeZstdTwoFrameTraceFile(path, recs));

    TraceRTLTraceReader reader(path, "unit.tracertl.twoframe");
    // Identity mapping (linear, no page align, window larger than any
    // address) so the strict sequential-PC order check below observes the
    // trace PCs directly.
    reader.setAddressMapping(0, 1ULL << 48, "linear", /*pageAlign*/false);
    ASSERT_TRUE(reader.init());

    int count = 0;
    uint64_t prev_pc = 0;
    while (true) {
        auto instr = reader.getNextInstruction();
        if (!instr.isValid()) break;
        // Strict sequential order across the frame boundary.
        if (count > 0) {
            EXPECT_EQ(instr.getPC(), prev_pc + 4) << "record " << count;
        }
        prev_pc = instr.getPC();
        ++count;
    }
    EXPECT_EQ(count, 100) << "both frames must drain fully";
    EXPECT_TRUE(reader.isEOF());
}

TEST(TraceRTLTraceReaderTest, ZstdTruncatedStreamDrainsAsEof)
{
    // A frame truncated mid-stream: the decompressor yields the fully
    // flushed prefix and then signals EOF (the same drain-as-EOF contract
    // the gzip/xz readers expose). Pinned here so the behavior is an
    // intentional contract, not an accident.
    std::vector<RTLInstr> recs;
    const uint64_t base = 0x561100000000ULL;
    for (int i = 0; i < 10000; ++i) {
        recs.push_back(makeRec(base + i * 4, ENC_ADDI));
    }
    const std::string path = "tracertl_truncated.bin.zstd";
    TraceFileGuard guard(path);
    ASSERT_TRUE(writeZstdTruncatedTraceFile(path, recs));

    TraceRTLTraceReader reader(path, "unit.tracertl.truncated");
    ASSERT_TRUE(reader.init());

    int count = 0;
    while (reader.getNextInstruction().isValid())
        ++count;
    // Some flushed prefix was served (zstd flushes at block granularity),
    // fewer than all records, and the reader reaches a clean EOF state.
    EXPECT_GT(count, 0);
    EXPECT_LT(count, 10000);
    EXPECT_TRUE(reader.isEOF());
}

TEST(TraceRTLTraceReaderTest, InitFailsForMalformedFiles)
{
    // Missing file.
    EXPECT_FALSE(TraceRTLTraceReader("definitely_missing_tracertl.bin",
                                     "unit.tracertl.missing").init());

    // Raw file whose size is not a multiple of 48.
    const std::string path = "tracertl_bad_size.bin";
    TraceFileGuard guard(path);
    std::ofstream ofs(path, std::ios::binary);
    ASSERT_TRUE(ofs.is_open());
    for (int i = 0; i < 10; ++i) {  // 10 bytes: neither 48 nor a multiple
        ofs.put(static_cast<char>(0xAB));
    }
    ofs.close();
    EXPECT_FALSE(TraceRTLTraceReader(path, "unit.tracertl.badsize").init());

    // Raw file with a single partial record (47 bytes).
    const std::string partial = "tracertl_partial.bin";
    TraceFileGuard partial_guard(partial);
    std::ofstream pofs(partial, std::ios::binary);
    ASSERT_TRUE(pofs.is_open());
    for (int i = 0; i < 47; ++i) {
        pofs.put(static_cast<char>(0));
    }
    pofs.close();
    EXPECT_FALSE(TraceRTLTraceReader(partial, "unit.tracertl.partial").init());
}

TEST(TraceRTLTraceReaderTest, FactoryCreatesTracertlAndNemuAlias)
{
    const std::string path = "tracertl_factory.bin";
    TraceFileGuard guard(path);
    ASSERT_TRUE(writeRawTraceFile(path, {makeRec(0x560F00000000ULL, ENC_ADDI)}));

    auto make = [&path](const std::string &fmt) {
        return gem5::o3::createTraceReader(fmt, path, "unit.factory");
    };
    auto tracertl = make("tracertl");
    ASSERT_NE(tracertl, nullptr);
    EXPECT_EQ(tracertl->getFormat(), "tracertl");
    ASSERT_TRUE(tracertl->init());

    auto nemu = make("nemu");
    ASSERT_NE(nemu, nullptr);
    EXPECT_EQ(nemu->getFormat(), "tracertl");
    ASSERT_TRUE(nemu->init());

    auto instr = tracertl->getNextInstruction();
    ASSERT_TRUE(instr.isValid());
    EXPECT_EQ(instr.getPC(), mapHash(0x560F00000000ULL));

    // Existing formats must keep working (no factory regression).
    auto champsim = make("champsim");
    EXPECT_NE(champsim, nullptr);
    EXPECT_EQ(make("unknown-format"), nullptr);
}
