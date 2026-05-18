/*
 * Copyright (c) 2026
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

#include <gtest/gtest.h>

#include "matrix/decoded_fifo.hh"
#include "matrix/detailed_cute_backend.hh"
#include "matrix/detailed_cute_scoreboard.hh"
#include "matrix/matrix_reg_resource.hh"

namespace gem5
{

namespace matrix
{

class DetailedCuteBackendTestProbe
{
  public:
    static size_t computeTaskCount(const DetailedCuteBackend &backend)
    {
        return backend.computeTasks.size();
    }

    static uint64_t backendStep(const DetailedCuteBackend &backend)
    {
        return backend.backendStep;
    }

    static bool computeUnitBusy(const DetailedCuteBackend &backend,
                                DetailedCuteBackend::ComputeUnitKind kind)
    {
        return backend.computeUnitBusyForTest(kind);
    }

    static DetailedCuteBackend::ComputeUnitKind
    activeComputeUnit(const DetailedCuteBackend &backend)
    {
        return backend.activeComputeUnitForTest();
    }

    static DetailedCuteBackend::MteTiming
    computeMteTiming(const DetailedCuteBackend &backend,
                     const AmuMmaDesc &desc)
    {
        return backend.computeMteTiming(desc);
    }
};

namespace
{

MatrixTensor
filledTensor(uint32_t rows, uint32_t cols, MatrixElemType elem_type,
             int64_t value)
{
    MatrixTensor tensor;
    tensor.rows = rows;
    tensor.cols = cols;
    tensor.elemType = elem_type;
    tensor.elements.assign(rows * cols, value);
    return tensor;
}

MatrixTensor
paddedTopLeftTensor(uint32_t rows, uint32_t cols, MatrixElemType elem_type,
                    uint32_t value_rows, uint32_t value_cols,
                    std::initializer_list<int64_t> values)
{
    MatrixTensor tensor = filledTensor(rows, cols, elem_type, 0);
    auto it = values.begin();
    for (uint32_t r = 0; r < value_rows; ++r) {
        for (uint32_t c = 0; c < value_cols; ++c) {
            tensor.elements[static_cast<size_t>(r) * cols + c] = *it++;
        }
    }
    return tensor;
}

void
expectTopLeft2x2(const MatrixTensor &tensor, int64_t v00, int64_t v01,
                 int64_t v10, int64_t v11)
{
    ASSERT_GE(tensor.rows, 2U);
    ASSERT_GE(tensor.cols, 2U);
    EXPECT_EQ(tensor.elements[0], v00);
    EXPECT_EQ(tensor.elements[1], v01);
    EXPECT_EQ(tensor.elements[tensor.cols], v10);
    EXPECT_EQ(tensor.elements[tensor.cols + 1], v11);
}

} // anonymous namespace

TEST(DecodedFifo, DecodeMmaNormalizesReadWriteSets)
{
    auto req = CuteRequest::makeMma(11, 3, 1, 2, 2, 2, 3);
    auto entry = decodeCuteRequest(req);

    EXPECT_TRUE(entry.isMma);
    EXPECT_EQ(entry.readRegs[0], 1);
    EXPECT_EQ(entry.readRegs[1], 2);
    EXPECT_EQ(entry.readRegs[2], 3);
    EXPECT_TRUE(entry.readValid[0]);
    EXPECT_TRUE(entry.readValid[1]);
    EXPECT_TRUE(entry.readValid[2]);
    EXPECT_EQ(entry.writeRegs[0], 3);
    EXPECT_TRUE(entry.writeValid[0]);
}

TEST(CuteRequest, EqualityTracksAllPayloadFields)
{
    CuteRequest lhs;
    lhs.seq = 7;
    lhs.kind = CuteRequestKind::Lsu;
    lhs.lsu.ms = 3;
    lhs.lsu.isStore = true;
    lhs.lsu.transpose = true;
    lhs.lsu.isAcc = true;
    lhs.lsu.baseAddr = 0x1000;
    lhs.lsu.physBaseAddr = 0x2000;
    lhs.lsu.stride = 64;
    lhs.lsu.row = 8;
    lhs.lsu.column = 16;
    lhs.lsu.elemType = MatrixElemType::Int32;

    auto rhs = lhs;
    EXPECT_TRUE(lhs == rhs);

    rhs.lsu.column = 32;
    EXPECT_FALSE(lhs == rhs);
}

TEST(CuteRequest, FactoryMethodsProduceStableBackendNativeRequests)
{
    auto release = CuteRequest::makeRelease(9, 5);
    EXPECT_EQ(release.seq, 9U);
    EXPECT_EQ(release.kind, CuteRequestKind::Release);
    EXPECT_EQ(release.release.tokenIndex, 5U);

    auto mma = CuteRequest::makeMma(7, 3, 1, 2, 8, 16, 32);
    EXPECT_EQ(mma.seq, 7U);
    EXPECT_EQ(mma.kind, CuteRequestKind::Mma);
    EXPECT_EQ(mma.mma.md, 3U);
    EXPECT_EQ(mma.mma.ms1, 1U);
    EXPECT_EQ(mma.mma.ms2, 2U);
    EXPECT_EQ(mma.mma.mtilem, 8U);
    EXPECT_EQ(mma.mma.mtilen, 16U);
    EXPECT_EQ(mma.mma.mtilek, 32U);
}

TEST(MatrixRegFile, StoresTensorAndAllocatedState)
{
    MatrixRegFile regfile(2, 2);
    MatrixTensor tensor = filledTensor(2, 2, MatrixElemType::Int32, 7);

    EXPECT_FALSE(regfile.hasAllocatedState());

    regfile.write(MatrixBankKind::A, 1, tensor);

    EXPECT_TRUE(regfile.allocated(MatrixBankKind::A, 1));
    EXPECT_TRUE(regfile.hasAllocatedState());
    EXPECT_EQ(regfile.read(MatrixBankKind::A, 1).elements[0], 7);
}

TEST(DetailedCuteBackend, ReportsCompletedMatrixRegisterStateForCheckpointGuard)
{
    DetailedCuteBackend backend;

    EXPECT_FALSE(backend.hasArchitecturalState());
    backend.matrixState().write(
        MatrixBankKind::C, 1, filledTensor(2, 2, MatrixElemType::Int32, 9));

    EXPECT_TRUE(backend.hasArchitecturalState());
    EXPECT_FALSE(backend.hasWork());
    EXPECT_FALSE(backend.hasCompletion());
}

TEST(MatrixRegResource, GrantsFullBankReadWithOneCycleResponse)
{
    MatrixRegResource resource;
    const auto read = MatrixRegResource::makeRead(
        MatrixBankKind::A, MatrixRegResource::Client::DataController, 7);

    const auto grants = resource.arbitrate({read});
    ASSERT_EQ(grants.size(), 1U);
    EXPECT_TRUE(grants[0].granted);
    EXPECT_EQ(grants[0].reason, MatrixRegResource::StallReason::None);
    EXPECT_FALSE(resource.readResponseReady(
        MatrixBankKind::A, MatrixRegResource::Client::DataController));

    resource.advanceCycle();
    EXPECT_TRUE(resource.readResponseReady(
        MatrixBankKind::A, MatrixRegResource::Client::DataController));
    EXPECT_TRUE(resource.consumeReadResponse(
        MatrixBankKind::A, MatrixRegResource::Client::DataController));
    EXPECT_FALSE(resource.readResponseReady(
        MatrixBankKind::A, MatrixRegResource::Client::DataController));
}

TEST(MatrixRegResource, RejectsPartialBankVectorAccess)
{
    MatrixRegResource resource;
    auto read = MatrixRegResource::makeRead(
        MatrixBankKind::B, MatrixRegResource::Client::DataController, 3);
    read.bankMask = 0x0f;

    const auto grants = resource.arbitrate({read});
    ASSERT_EQ(grants.size(), 1U);
    EXPECT_FALSE(grants[0].granted);
    EXPECT_EQ(grants[0].reason,
              MatrixRegResource::StallReason::PartialBankMask);
}

TEST(MatrixRegResource, AbLoaderWritePriorityStallsDataControllerRead)
{
    MatrixRegResource resource;
    const auto write = MatrixRegResource::makeWrite(
        MatrixBankKind::A, MatrixRegResource::Client::MemoryLoader, 4);
    const auto read = MatrixRegResource::makeRead(
        MatrixBankKind::A, MatrixRegResource::Client::DataController, 4);

    const auto grants = resource.arbitrate({write, read});
    ASSERT_EQ(grants.size(), 2U);
    EXPECT_TRUE(grants[0].granted);
    EXPECT_EQ(grants[0].reason, MatrixRegResource::StallReason::None);
    EXPECT_FALSE(grants[1].granted);
    EXPECT_EQ(grants[1].reason,
              MatrixRegResource::StallReason::AbWritePriority);

    resource.advanceCycle();
    EXPECT_FALSE(resource.readResponseReady(
        MatrixBankKind::A, MatrixRegResource::Client::DataController));
}

TEST(MatrixRegResource, CReadWriteSameParityConflictStalls)
{
    MatrixRegResource resource;
    const auto read = MatrixRegResource::makeRead(
        MatrixBankKind::C, MatrixRegResource::Client::DataController, 2);
    const auto write = MatrixRegResource::makeWrite(
        MatrixBankKind::C, MatrixRegResource::Client::MemoryLoader, 4);

    const auto grants = resource.arbitrate({read, write});
    ASSERT_EQ(grants.size(), 2U);
    EXPECT_FALSE(grants[0].granted);
    EXPECT_EQ(grants[0].reason,
              MatrixRegResource::StallReason::CReadWriteConflict);
    EXPECT_FALSE(grants[1].granted);
    EXPECT_EQ(grants[1].reason,
              MatrixRegResource::StallReason::CReadWriteConflict);

    resource.advanceCycle();
    EXPECT_FALSE(resource.readResponseReady(
        MatrixBankKind::C, MatrixRegResource::Client::DataController));
}

TEST(MatrixRegResource, CReadWriteOppositeParityCanShareCycle)
{
    MatrixRegResource resource;
    const auto read = MatrixRegResource::makeRead(
        MatrixBankKind::C, MatrixRegResource::Client::DataController, 2);
    const auto write = MatrixRegResource::makeWrite(
        MatrixBankKind::C, MatrixRegResource::Client::MemoryLoader, 3);

    const auto grants = resource.arbitrate({read, write});
    ASSERT_EQ(grants.size(), 2U);
    EXPECT_TRUE(grants[0].granted);
    EXPECT_TRUE(grants[1].granted);

    resource.advanceCycle();
    EXPECT_TRUE(resource.readResponseReady(
        MatrixBankKind::C, MatrixRegResource::Client::DataController));
}

TEST(DetailedCuteScoreboard, LoadReserveBlocksDependentComputeUntilCompletion)
{
    DetailedCuteScoreboard scoreboard(4, 4);

    CuteRequest load_a;
    load_a.seq = 1;
    load_a.kind = CuteRequestKind::Lsu;
    load_a.lsu.ms = 0;
    load_a.lsu.isStore = false;
    load_a.lsu.isA = true;
    auto load_entry = decodeCuteRequest(load_a);

    auto mma_req = CuteRequest::makeMma(2, 3, 0, 1, 2, 2, 3);
    auto mma_entry = decodeCuteRequest(mma_req);

    EXPECT_TRUE(scoreboard.canIssue(load_entry));
    scoreboard.onIssue(load_entry);

    EXPECT_TRUE(scoreboard.fuBusyForTest(DetailedCuteScoreboard::FuKind::AML));
    EXPECT_TRUE(scoreboard.regBusyForTest(0, MatrixBankKind::A));
    EXPECT_FALSE(scoreboard.canIssue(mma_entry));

    scoreboard.onLoadFinish(load_entry);
    EXPECT_FALSE(scoreboard.regBusyForTest(0, MatrixBankKind::A));
    EXPECT_TRUE(scoreboard.canIssue(mma_entry));
}

TEST(DetailedCuteScoreboard, StoreReadFinishAndWriteFinishSplitCorrectly)
{
    DetailedCuteScoreboard scoreboard(4, 4);

    CuteRequest store_c;
    store_c.seq = 1;
    store_c.kind = CuteRequestKind::Lsu;
    store_c.lsu.ms = 2;
    store_c.lsu.isStore = true;
    store_c.lsu.isAcc = true;
    auto store_entry = decodeCuteRequest(store_c);

    auto mma_entry = decodeCuteRequest(CuteRequest::makeMma(
        2, 2, 0, 1, 2, 2, 2));

    EXPECT_TRUE(scoreboard.canIssue(store_entry));
    scoreboard.onIssue(store_entry);

    EXPECT_EQ(scoreboard.pendingReadersForTest(2, MatrixBankKind::C), 1U);
    EXPECT_FALSE(scoreboard.canIssue(mma_entry));

    scoreboard.onStoreReadFinish(store_entry);
    EXPECT_EQ(scoreboard.pendingReadersForTest(2, MatrixBankKind::C), 0U);
    EXPECT_TRUE(scoreboard.fuBusyForTest(DetailedCuteScoreboard::FuKind::CML));
    EXPECT_TRUE(scoreboard.canIssue(mma_entry));

    scoreboard.onStoreWriteFinish(store_entry);
    EXPECT_FALSE(scoreboard.fuBusyForTest(DetailedCuteScoreboard::FuKind::CML));
}

TEST(DetailedCuteScoreboard, ComputeLifecycleClearsReadersInStages)
{
    DetailedCuteScoreboard scoreboard(4, 4);

    CuteRequest load_a;
    load_a.seq = 1;
    load_a.kind = CuteRequestKind::Lsu;
    load_a.lsu.ms = 0;
    load_a.lsu.isStore = false;
    load_a.lsu.isA = true;
    auto load_entry = decodeCuteRequest(load_a);
    scoreboard.onLoadIssue(load_entry);
    scoreboard.onLoadFinish(load_entry);

    CuteRequest load_b;
    load_b.seq = 2;
    load_b.kind = CuteRequestKind::Lsu;
    load_b.lsu.ms = 1;
    load_b.lsu.isStore = false;
    load_b.lsu.isB = true;
    auto load_b_entry = decodeCuteRequest(load_b);
    scoreboard.onLoadIssue(load_b_entry);
    scoreboard.onLoadFinish(load_b_entry);

    auto zero_c = decodeCuteRequest(
        CuteRequest::makeArithZero(
            3, MatrixBankKind::C, 2, 2, 2, MatrixElemType::Int32));
    scoreboard.onArithIssue(zero_c);
    scoreboard.onArithFinish(zero_c);

    auto mma_entry = decodeCuteRequest(CuteRequest::makeMma(4, 2, 0, 1, 2, 2, 3));

    EXPECT_TRUE(scoreboard.canIssue(mma_entry));
    scoreboard.onComputeIssue(mma_entry);
    EXPECT_EQ(scoreboard.pendingReadersForTest(0, MatrixBankKind::A), 1U);
    EXPECT_EQ(scoreboard.pendingReadersForTest(1, MatrixBankKind::B), 1U);
    EXPECT_EQ(scoreboard.pendingReadersForTest(2, MatrixBankKind::C), 1U);
    EXPECT_TRUE(scoreboard.regBusyForTest(2, MatrixBankKind::C));

    scoreboard.onComputeReadFinishA(mma_entry);
    EXPECT_EQ(scoreboard.pendingReadersForTest(0, MatrixBankKind::A), 0U);
    EXPECT_EQ(scoreboard.pendingReadersForTest(1, MatrixBankKind::B), 1U);
    EXPECT_EQ(scoreboard.pendingReadersForTest(2, MatrixBankKind::C), 1U);
    EXPECT_TRUE(scoreboard.regBusyForTest(2, MatrixBankKind::C));

    scoreboard.onComputeReadFinishB(mma_entry);
    EXPECT_EQ(scoreboard.pendingReadersForTest(1, MatrixBankKind::B), 0U);
    EXPECT_EQ(scoreboard.pendingReadersForTest(2, MatrixBankKind::C), 1U);
    EXPECT_TRUE(scoreboard.regBusyForTest(2, MatrixBankKind::C));

    scoreboard.onComputeWriteFinishC(mma_entry);
    EXPECT_EQ(scoreboard.pendingReadersForTest(2, MatrixBankKind::C), 0U);
    EXPECT_FALSE(scoreboard.regBusyForTest(2, MatrixBankKind::C));
}

TEST(DetailedCuteBackend, QueueDepthDefaultsToEight)
{
    DetailedCuteBackend backend;

    for (uint64_t i = 0; i < 8; ++i) {
        auto req = CuteRequest::makeRelease(i + 1, 0);
        EXPECT_TRUE(backend.canAccept(req));
        backend.submit(req);
    }

    auto req9 = CuteRequest::makeRelease(9, 0);
    EXPECT_FALSE(backend.canAccept(req9));
}

TEST(DetailedCuteBackend, DefaultBackendAcceptsArchitecturalRegIndexFour)
{
    DetailedCuteBackend backend;

    backend.submit(CuteRequest::makeMma(1, 4, 1, 2, 2, 2, 3));

    EXPECT_NO_THROW(backend.step());
}

TEST(DetailedCuteBackend, MaintainsRequestOrder)
{
    DetailedCuteBackend backend;

    backend.submit(CuteRequest::makeArithZero(
        1, MatrixBankKind::A, 0, 2, 2, MatrixElemType::Int8));
    backend.submit(CuteRequest::makeRelease(2, 1));

    while (backend.hasWork()) {
        backend.step();
    }

    ASSERT_TRUE(backend.hasCompletion());
    auto first = backend.popCompletion();
    ASSERT_TRUE(backend.hasCompletion());
    auto second = backend.popCompletion();

    EXPECT_EQ(first.seq, 1U);
    EXPECT_EQ(second.seq, 2U);
}

TEST(DetailedCuteBackend, ReleaseCompletionOnlyAppearsAtTerminalStage)
{
    DetailedCuteBackend backend;

    backend.submit(CuteRequest::makeRelease(1, 3));

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());

    backend.step();
    ASSERT_TRUE(backend.hasCompletion());
    auto completion = backend.popCompletion();
    EXPECT_EQ(completion.seq, 1U);
    EXPECT_TRUE(completion.hasTokenRelease);
    EXPECT_EQ(completion.tokenIdx, 3U);
}

TEST(DetailedCuteBackend, ReleaseWaitsForPendingStoreCompletion)
{
    auto memory = std::make_unique<SparseMatrixMemoryAdapter>();
    auto *memory_ptr = memory.get();
    DetailedCuteBackend backend(std::move(memory));

    backend.matrixState().write(
        MatrixBankKind::C, 3,
        MatrixTensor{2, 2, MatrixElemType::Int32, {10, 20, 30, 40}});

    CuteRequest store_c;
    store_c.seq = 1;
    store_c.kind = CuteRequestKind::Lsu;
    store_c.lsu.ms = 3;
    store_c.lsu.isStore = true;
    store_c.lsu.isAcc = true;
    store_c.lsu.baseAddr = 0x3000;
    store_c.lsu.stride = 8;
    store_c.lsu.row = 2;
    store_c.lsu.column = 2;
    store_c.lsu.elemType = MatrixElemType::Int32;

    auto release = CuteRequest::makeRelease(2, 7);

    backend.submit(store_c);
    backend.submit(release);

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());
    EXPECT_TRUE(backend.hasWork());

    backend.step();
    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 1U);
    EXPECT_TRUE(backend.hasWork());

    EXPECT_FALSE(backend.hasCompletion());

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());
    backend.step();
    ASSERT_TRUE(backend.hasCompletion());
    auto completion = backend.popCompletion();
    EXPECT_EQ(completion.seq, 2U);
    EXPECT_TRUE(completion.hasTokenRelease);

    int64_t value = 0;
    ASSERT_TRUE(memory_ptr->readElement(0x3000, value));
    EXPECT_EQ(value, 10);
}

TEST(DetailedCuteBackend, ReleaseWaitsForBackendDrainBeyondPendingStoreCount)
{
    auto memory = std::make_unique<SparseMatrixMemoryAdapter>();
    auto *memory_ptr = memory.get();
    memory_ptr->writeElement(0x1000, 1);
    memory_ptr->writeElement(0x1001, 2);
    memory_ptr->writeElement(0x1004, 3);
    memory_ptr->writeElement(0x1005, 4);

    DetailedCuteBackend backend(std::move(memory));

    CuteRequest load_a;
    load_a.seq = 1;
    load_a.kind = CuteRequestKind::Lsu;
    load_a.lsu.ms = 1;
    load_a.lsu.isStore = false;
    load_a.lsu.isA = true;
    load_a.lsu.baseAddr = 0x1000;
    load_a.lsu.stride = 4;
    load_a.lsu.row = 2;
    load_a.lsu.column = 2;
    load_a.lsu.elemType = MatrixElemType::Int8;

    auto release = CuteRequest::makeRelease(2, 5);

    backend.submit(load_a);
    backend.submit(release);

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());
    EXPECT_EQ(backend.traceCounters().fifoBlock, 0U);

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());
    EXPECT_EQ(backend.traceCounters().fifoBlock, 1U);

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());
    EXPECT_EQ(backend.traceCounters().fifoBlock, 2U);

    backend.step();
    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 1U);
    EXPECT_FALSE(backend.hasCompletion());
    EXPECT_EQ(backend.traceCounters().fifoBlock, 3U);

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());

    backend.step();
    ASSERT_TRUE(backend.hasCompletion());
    auto completion = backend.popCompletion();
    EXPECT_EQ(completion.seq, 2U);
    EXPECT_TRUE(completion.hasTokenRelease);
}

TEST(DetailedCuteBackend, ReleaseWaitsForComputeTerminalCompletion)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        paddedTopLeftTensor(
            2, 32, MatrixElemType::Int8, 2, 3,
            {1, 2, 3, 4, 5, 6}));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        paddedTopLeftTensor(
            32, 128, MatrixElemType::Int8, 3, 2,
            {7, 8, 9, 10, 11, 12}));
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        filledTensor(2, 128, MatrixElemType::Int32, 0));

    backend.submit(CuteRequest::makeMma(1, 3, 1, 2, 2, 128, 32));
    backend.submit(CuteRequest::makeRelease(2, 9));

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);

    for (int i = 0; i < 4; ++i) {
        backend.step();
        EXPECT_FALSE(backend.hasCompletion());
        EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    }

    while (!backend.hasCompletion()) {
        backend.step();
        EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    }
    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 1U);
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 2U);

    backend.step();
    ASSERT_TRUE(backend.hasCompletion());
    auto completion = backend.popCompletion();
    EXPECT_EQ(completion.seq, 2U);
    EXPECT_TRUE(completion.hasTokenRelease);
    EXPECT_EQ(completion.tokenIdx, 9U);
}

TEST(DetailedCuteBackend, StoreSnapshotsRegisterDataAtReadFinish)
{
    auto memory = std::make_unique<SparseMatrixMemoryAdapter>();
    auto *memory_ptr = memory.get();
    DetailedCuteBackend backend(std::move(memory));

    backend.matrixState().write(
        MatrixBankKind::C, 3,
        MatrixTensor{2, 2, MatrixElemType::Int32, {10, 20, 30, 40}});

    CuteRequest store_c;
    store_c.seq = 1;
    store_c.kind = CuteRequestKind::Lsu;
    store_c.lsu.ms = 3;
    store_c.lsu.isStore = true;
    store_c.lsu.isAcc = true;
    store_c.lsu.baseAddr = 0x3000;
    store_c.lsu.stride = 8;
    store_c.lsu.row = 2;
    store_c.lsu.column = 2;
    store_c.lsu.elemType = MatrixElemType::Int32;

    backend.submit(store_c);

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());
    EXPECT_TRUE(backend.hasWork());

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());
    EXPECT_TRUE(backend.hasWork());

    backend.matrixState().write(
        MatrixBankKind::C, 3,
        MatrixTensor{2, 2, MatrixElemType::Int32, {100, 200, 300, 400}});

    backend.step();
    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 1U);
    EXPECT_FALSE(backend.hasCompletion());

    int64_t value = 0;
    ASSERT_TRUE(memory_ptr->readElement(0x3000, value));
    EXPECT_EQ(value, 10);
    ASSERT_TRUE(memory_ptr->readElement(0x3008, value));
    EXPECT_EQ(value, 30);
}

TEST(DetailedCuteBackend, LoadSnapshotsMemoryDataBeforeRegWrite)
{
    auto memory = std::make_unique<SparseMatrixMemoryAdapter>();
    auto *memory_ptr = memory.get();
    memory_ptr->writeElement(0x1000, 1);
    memory_ptr->writeElement(0x1001, 2);
    memory_ptr->writeElement(0x1004, 3);
    memory_ptr->writeElement(0x1005, 4);

    DetailedCuteBackend backend(std::move(memory));

    CuteRequest load_a;
    load_a.seq = 1;
    load_a.kind = CuteRequestKind::Lsu;
    load_a.lsu.ms = 1;
    load_a.lsu.isStore = false;
    load_a.lsu.isA = true;
    load_a.lsu.baseAddr = 0x1000;
    load_a.lsu.stride = 4;
    load_a.lsu.row = 2;
    load_a.lsu.column = 2;
    load_a.lsu.elemType = MatrixElemType::Int8;

    backend.submit(load_a);

    backend.step();
    backend.step();
    backend.step();

    memory_ptr->writeElement(0x1000, 9);
    memory_ptr->writeElement(0x1001, 9);
    memory_ptr->writeElement(0x1004, 9);
    memory_ptr->writeElement(0x1005, 9);

    backend.step();

    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 1U);
    const auto &tensor = backend.matrixState().read(MatrixBankKind::A, 1);
    EXPECT_EQ(tensor.elements[0], 1);
    EXPECT_EQ(tensor.elements[1], 2);
    EXPECT_EQ(tensor.elements[2], 3);
    EXPECT_EQ(tensor.elements[3], 4);
}

TEST(DetailedCuteBackend, UnsupportedLoadStillReachesTerminalCompletion)
{
    DetailedCuteBackend backend;

    CuteRequest load_a;
    load_a.seq = 1;
    load_a.kind = CuteRequestKind::Lsu;
    load_a.lsu.ms = 1;
    load_a.lsu.isStore = false;
    load_a.lsu.isA = true;
    load_a.lsu.baseAddr = 0x1000;
    load_a.lsu.stride = 4;
    load_a.lsu.row = 2;
    load_a.lsu.column = 2;
    load_a.lsu.elemType = MatrixElemType::Int8;

    backend.submit(load_a);

    while (backend.hasWork()) {
        backend.step();
    }

    ASSERT_TRUE(backend.hasCompletion());
    auto completion = backend.popCompletion();
    EXPECT_EQ(completion.seq, 1U);
    EXPECT_EQ(completion.kind, CuteRequestKind::Lsu);
    EXPECT_EQ(completion.status, CuteCompletionStatus::Unsupported);
    EXPECT_FALSE(backend.matrixState().allocated(MatrixBankKind::A, 1));
}

TEST(DetailedCuteBackend, ComputeWriteOccursOnlyAtTerminalStage)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        paddedTopLeftTensor(
            2, 32, MatrixElemType::Int8, 2, 3,
            {1, 2, 3, 4, 5, 6}));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        paddedTopLeftTensor(
            32, 128, MatrixElemType::Int8, 3, 2,
            {7, 8, 9, 10, 11, 12}));
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        filledTensor(2, 128, MatrixElemType::Int32, 0));

    backend.submit(CuteRequest::makeMma(1, 3, 1, 2, 2, 128, 32));

    while (!DetailedCuteBackendTestProbe::computeUnitBusy(
        backend, DetailedCuteBackend::ComputeUnitKind::CDC)) {
        backend.step();
        EXPECT_FALSE(backend.hasCompletion());
        auto c = backend.matrixState().read(MatrixBankKind::C, 3);
        EXPECT_EQ(c.elements[0], 0);
        EXPECT_EQ(c.elements[c.cols + 1], 0);
    }

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());
    auto c = backend.matrixState().read(MatrixBankKind::C, 3);
    expectTopLeft2x2(c, 58, 64, 139, 154);

    while (!backend.hasCompletion()) {
        backend.step();
    }
    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 1U);
    c = backend.matrixState().read(MatrixBankKind::C, 3);
    expectTopLeft2x2(c, 58, 64, 139, 154);
}

TEST(DetailedCuteBackend, ComputeKeepsCDestBusyUntilWriteFinishAndCompletion)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        filledTensor(4, 64, MatrixElemType::Int8, 1));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        filledTensor(64, 128, MatrixElemType::Int8, 1));
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        filledTensor(4, 128, MatrixElemType::Int32, 0));

    backend.submit(CuteRequest::makeMma(1, 3, 1, 2, 4, 128, 64));
    backend.submit(CuteRequest::makeArithZero(
        2, MatrixBankKind::C, 3, 4, 4, MatrixElemType::Int32));

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    EXPECT_EQ(backend.traceCounters().scoreboardBlock, 0U);

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    EXPECT_EQ(backend.traceCounters().scoreboardBlock, 1U);
    EXPECT_EQ(
        backend.traceCounters().scoreboardBlockReasons[
            static_cast<size_t>(DetailedCuteScoreboard::BlockReason::DestBusy)],
        1U);

    while (!DetailedCuteBackendTestProbe::computeUnitBusy(
        backend, DetailedCuteBackend::ComputeUnitKind::CDC)) {
        backend.step();
    }
    EXPECT_EQ(
        DetailedCuteBackendTestProbe::activeComputeUnit(backend),
        DetailedCuteBackend::ComputeUnitKind::CDC);
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    EXPECT_GE(backend.traceCounters().scoreboardBlockReasons[
                  static_cast<size_t>(
                      DetailedCuteScoreboard::BlockReason::DestBusy)],
              3U);
    EXPECT_FALSE(backend.hasCompletion());
    EXPECT_EQ(DetailedCuteBackendTestProbe::computeTaskCount(backend), 1U);

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    EXPECT_EQ(DetailedCuteBackendTestProbe::computeTaskCount(backend), 1U);
    EXPECT_FALSE(backend.hasCompletion());

    while (!backend.hasCompletion()) {
        backend.step();
        EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    }
    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 1U);

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 2U);
}

TEST(DetailedCuteBackend, ComputeSubUnitsAreTrackedExplicitly)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        filledTensor(4, 64, MatrixElemType::Int8, 1));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        filledTensor(64, 128, MatrixElemType::Int8, 1));
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        filledTensor(4, 128, MatrixElemType::Int32, 0));

    backend.submit(CuteRequest::makeMma(1, 3, 1, 2, 4, 128, 64));

    backend.step();
    EXPECT_EQ(
        DetailedCuteBackendTestProbe::activeComputeUnit(backend),
        DetailedCuteBackend::ComputeUnitKind::ADC);
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::ADC));
    EXPECT_EQ(
        backend.traceCounters().computeUnitIssuesByKind[
            static_cast<size_t>(DetailedCuteBackend::ComputeUnitKind::ADC)],
        1U);
    EXPECT_EQ(
        backend.traceCounters().computeUnitFinishesByKind[
            static_cast<size_t>(DetailedCuteBackend::ComputeUnitKind::ADC)],
        0U);

    backend.step();
    EXPECT_EQ(
        DetailedCuteBackendTestProbe::activeComputeUnit(backend),
        DetailedCuteBackend::ComputeUnitKind::BDC);
    EXPECT_FALSE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::ADC));
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::BDC));
    EXPECT_EQ(
        backend.traceCounters().computeUnitFinishesByKind[
            static_cast<size_t>(DetailedCuteBackend::ComputeUnitKind::ADC)],
        1U);

    EXPECT_EQ(
        DetailedCuteBackendTestProbe::activeComputeUnit(backend),
        DetailedCuteBackend::ComputeUnitKind::BDC);
    EXPECT_EQ(
        backend.traceCounters().computeUnitIssuesByKind[
            static_cast<size_t>(DetailedCuteBackend::ComputeUnitKind::BDC)],
        1U);

    backend.step();
    EXPECT_EQ(
        DetailedCuteBackendTestProbe::activeComputeUnit(backend),
        DetailedCuteBackend::ComputeUnitKind::MTE);
    EXPECT_FALSE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::BDC));
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::MTE));
    EXPECT_EQ(
        backend.traceCounters().computeUnitFinishesByKind[
            static_cast<size_t>(DetailedCuteBackend::ComputeUnitKind::BDC)],
        1U);

    backend.step();
    EXPECT_EQ(
        DetailedCuteBackendTestProbe::activeComputeUnit(backend),
        DetailedCuteBackend::ComputeUnitKind::MTE);
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::MTE));

    while (!DetailedCuteBackendTestProbe::computeUnitBusy(
        backend, DetailedCuteBackend::ComputeUnitKind::CDC)) {
        EXPECT_TRUE(
            DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::MTE));
        backend.step();
    }
    EXPECT_EQ(
        DetailedCuteBackendTestProbe::activeComputeUnit(backend),
        DetailedCuteBackend::ComputeUnitKind::CDC);
    EXPECT_FALSE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::MTE));
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::CDC));
    EXPECT_EQ(
        backend.traceCounters().computeUnitIssuesByKind[
            static_cast<size_t>(DetailedCuteBackend::ComputeUnitKind::MTE)],
        1U);
    EXPECT_EQ(
        backend.traceCounters().computeUnitFinishesByKind[
            static_cast<size_t>(DetailedCuteBackend::ComputeUnitKind::MTE)],
        1U);

    EXPECT_EQ(DetailedCuteBackendTestProbe::computeTaskCount(backend), 1U);
    EXPECT_TRUE(backend.hasWork());
    EXPECT_FALSE(backend.hasCompletion());

    backend.step();
    EXPECT_EQ(DetailedCuteBackendTestProbe::computeTaskCount(backend), 1U);
    EXPECT_EQ(
        DetailedCuteBackendTestProbe::activeComputeUnit(backend),
        DetailedCuteBackend::ComputeUnitKind::CDC);
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::CDC));
    EXPECT_FALSE(backend.hasCompletion());

    while (backend.hasWork()) {
        backend.step();
    }

    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 1U);
    EXPECT_EQ(
        backend.traceCounters().computeUnitIssuesByKind[
            static_cast<size_t>(DetailedCuteBackend::ComputeUnitKind::CDC)],
        1U);
    EXPECT_EQ(
        backend.traceCounters().computeUnitFinishesByKind[
            static_cast<size_t>(DetailedCuteBackend::ComputeUnitKind::CDC)],
        1U);
}

TEST(DetailedCuteBackend, MultipleComputeTasksCanOverlapFrontUnits)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        filledTensor(4, 64, MatrixElemType::Int8, 1));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        filledTensor(64, 128, MatrixElemType::Int8, 1));
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        filledTensor(4, 128, MatrixElemType::Int32, 0));
    backend.matrixState().write(
        MatrixBankKind::A, 0,
        filledTensor(4, 64, MatrixElemType::Int8, 2));
    backend.matrixState().write(
        MatrixBankKind::B, 0,
        filledTensor(64, 128, MatrixElemType::Int8, 2));
    backend.matrixState().write(
        MatrixBankKind::C, 1,
        filledTensor(4, 128, MatrixElemType::Int32, 0));

    backend.submit(CuteRequest::makeMma(1, 3, 1, 2, 4, 128, 64));
    backend.submit(CuteRequest::makeMma(2, 1, 0, 0, 4, 128, 64));

    backend.step();
    EXPECT_EQ(DetailedCuteBackendTestProbe::computeTaskCount(backend), 1U);
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::ADC));

    backend.step();
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::BDC));
    EXPECT_EQ(DetailedCuteBackendTestProbe::computeTaskCount(backend), 1U);

    backend.step();
    EXPECT_EQ(DetailedCuteBackendTestProbe::computeTaskCount(backend), 1U);
    EXPECT_FALSE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::ADC));
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::MTE));

    backend.step();
    EXPECT_EQ(DetailedCuteBackendTestProbe::computeTaskCount(backend), 2U);
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::ADC));
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::MTE));

    backend.step();
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::BDC));

    while (backend.hasWork()) {
        backend.step();
    }

    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 1U);
    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 2U);
}

TEST(DetailedCuteBackend,
     SecondComputeWaitsOnAdcBdcCdcAvailabilityNotScoreboardComputeBusy)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        filledTensor(4, 64, MatrixElemType::Int8, 1));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        filledTensor(64, 128, MatrixElemType::Int8, 1));
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        filledTensor(4, 128, MatrixElemType::Int32, 0));
    backend.matrixState().write(
        MatrixBankKind::A, 0,
        filledTensor(4, 64, MatrixElemType::Int8, 2));
    backend.matrixState().write(
        MatrixBankKind::B, 0,
        filledTensor(64, 128, MatrixElemType::Int8, 2));
    backend.matrixState().write(
        MatrixBankKind::C, 1,
        filledTensor(4, 128, MatrixElemType::Int32, 0));

    backend.submit(CuteRequest::makeMma(1, 3, 1, 2, 4, 128, 64));
    backend.submit(CuteRequest::makeMma(2, 1, 0, 0, 4, 128, 64));

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    EXPECT_EQ(backend.traceCounters().fifoBlock, 0U);
    EXPECT_EQ(backend.traceCounters().scoreboardBlock, 0U);
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::ADC));

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    EXPECT_EQ(backend.traceCounters().fifoBlock, 1U);
    EXPECT_EQ(
        backend.traceCounters().fifoBlockReasons[
            static_cast<size_t>(
                DetailedCuteBackend::FifoBlockReason::DownstreamNotAccepting)],
        1U);
    EXPECT_EQ(backend.traceCounters().scoreboardBlock, 0U);
    EXPECT_FALSE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::ADC));
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::BDC));

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    EXPECT_EQ(backend.traceCounters().fifoBlock, 2U);
    EXPECT_EQ(backend.traceCounters().scoreboardBlock, 0U);
    EXPECT_FALSE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::ADC));
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::MTE));

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 2U);
    EXPECT_EQ(backend.traceCounters().fifoBlock, 2U);
    EXPECT_EQ(backend.traceCounters().scoreboardBlock, 0U);
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::ADC));
    EXPECT_TRUE(
        DetailedCuteBackendTestProbe::computeUnitBusy(backend, DetailedCuteBackend::ComputeUnitKind::MTE));
}

TEST(DetailedCuteBackend, ComputePathRequiresAdcBdcCdcReady)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        filledTensor(4, 64, MatrixElemType::Int8, 1));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        filledTensor(64, 128, MatrixElemType::Int8, 1));
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        filledTensor(4, 128, MatrixElemType::Int32, 0));
    backend.matrixState().write(
        MatrixBankKind::A, 0,
        filledTensor(4, 64, MatrixElemType::Int8, 2));
    backend.matrixState().write(
        MatrixBankKind::B, 0,
        filledTensor(64, 128, MatrixElemType::Int8, 2));
    backend.matrixState().write(
        MatrixBankKind::C, 1,
        filledTensor(4, 128, MatrixElemType::Int32, 0));

    backend.submit(CuteRequest::makeMma(1, 3, 1, 2, 4, 128, 64));
    backend.submit(CuteRequest::makeMma(2, 1, 0, 0, 4, 128, 64));

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    EXPECT_TRUE(DetailedCuteBackendTestProbe::computeUnitBusy(
        backend, DetailedCuteBackend::ComputeUnitKind::ADC));

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    EXPECT_TRUE(DetailedCuteBackendTestProbe::computeUnitBusy(
        backend, DetailedCuteBackend::ComputeUnitKind::BDC));

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    EXPECT_FALSE(DetailedCuteBackendTestProbe::computeUnitBusy(
        backend, DetailedCuteBackend::ComputeUnitKind::ADC));
    EXPECT_TRUE(DetailedCuteBackendTestProbe::computeUnitBusy(
        backend, DetailedCuteBackend::ComputeUnitKind::MTE));

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 2U);
    EXPECT_TRUE(DetailedCuteBackendTestProbe::computeUnitBusy(
        backend, DetailedCuteBackend::ComputeUnitKind::ADC));
    EXPECT_TRUE(DetailedCuteBackendTestProbe::computeUnitBusy(
        backend, DetailedCuteBackend::ComputeUnitKind::MTE));

    while (!DetailedCuteBackendTestProbe::computeUnitBusy(
        backend, DetailedCuteBackend::ComputeUnitKind::CDC)) {
        backend.step();
        EXPECT_EQ(backend.traceCounters().fifoDequeue, 2U);
    }

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 2U);
    EXPECT_TRUE(DetailedCuteBackendTestProbe::computeUnitBusy(
        backend, DetailedCuteBackend::ComputeUnitKind::CDC));

    while (backend.hasWork()) {
        backend.step();
    }

    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 1U);
    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 2U);
}

TEST(DetailedCuteBackend, ComputeSnapshotsABBeforeWriteback)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        paddedTopLeftTensor(
            2, 32, MatrixElemType::Int8, 2, 3,
            {1, 2, 3, 4, 5, 6}));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        paddedTopLeftTensor(
            32, 128, MatrixElemType::Int8, 3, 2,
            {7, 8, 9, 10, 11, 12}));
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        filledTensor(2, 128, MatrixElemType::Int32, 0));

    backend.submit(CuteRequest::makeMma(1, 3, 1, 2, 2, 128, 32));

    backend.step();
    backend.step();
    backend.step();

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        filledTensor(2, 32, MatrixElemType::Int8, 9));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        filledTensor(32, 128, MatrixElemType::Int8, 1));

    while (backend.hasWork()) {
        backend.step();
    }

    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().status, CuteCompletionStatus::Success);
    auto c = backend.matrixState().read(MatrixBankKind::C, 3);
    expectTopLeft2x2(c, 58, 64, 139, 154);
}

TEST(DetailedCuteBackend, MmaccIntegerEncodingSucceeds)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        paddedTopLeftTensor(
            2, 32, MatrixElemType::Int8, 2, 2,
            {1, 2, 3, 4}));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        paddedTopLeftTensor(
            32, 128, MatrixElemType::Int8, 2, 2,
            {5, 6, 7, 8}));
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        filledTensor(2, 128, MatrixElemType::Int32, 0));

    auto mma = CuteRequest::makeMma(1, 3, 1, 2, 2, 128, 32);
    mma.mma.types1 = 0x4;
    mma.mma.types2 = 0x4;
    mma.mma.typed = 0x2;
    backend.submit(mma);

    while (backend.hasWork()) {
        backend.step();
    }

    ASSERT_TRUE(backend.hasCompletion());
    auto completion = backend.popCompletion();
    EXPECT_EQ(completion.status, CuteCompletionStatus::Success);
    const auto &c = backend.matrixState().read(MatrixBankKind::C, 3);
    expectTopLeft2x2(c, 19, 22, 43, 50);
}

TEST(DetailedCuteBackend, Int16IntegerMmaEncodingSucceeds)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        paddedTopLeftTensor(
            2, 32, MatrixElemType::Int16, 2, 2,
            {1, 2, 3, 4}));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        paddedTopLeftTensor(
            32, 128, MatrixElemType::Int16, 2, 2,
            {5, 6, 7, 8}));
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        filledTensor(2, 128, MatrixElemType::Int32, 0));

    auto mma = CuteRequest::makeMma(1, 3, 1, 2, 2, 128, 32);
    mma.mma.types1 = 0x1;
    mma.mma.types2 = 0x1;
    mma.mma.typed = 0x2;
    backend.submit(mma);

    while (backend.hasWork()) {
        backend.step();
    }

    ASSERT_TRUE(backend.hasCompletion());
    auto completion = backend.popCompletion();
    EXPECT_EQ(completion.status, CuteCompletionStatus::Success);
    const auto &c = backend.matrixState().read(MatrixBankKind::C, 3);
    expectTopLeft2x2(c, 19, 22, 43, 50);
}

TEST(DetailedCuteBackend, ComputeLatencyFollowsActiveRtlKGroups)
{
    DetailedCuteBackend short_k_backend;
    short_k_backend.matrixState().write(
        MatrixBankKind::A, 1, filledTensor(4, 32, MatrixElemType::Int8, 1));
    short_k_backend.matrixState().write(
        MatrixBankKind::B, 2, filledTensor(32, 128, MatrixElemType::Int8, 1));
    short_k_backend.matrixState().write(
        MatrixBankKind::C, 3, filledTensor(4, 128, MatrixElemType::Int32, 0));
    short_k_backend.submit(CuteRequest::makeMma(1, 3, 1, 2, 4, 128, 32));

    while (short_k_backend.hasWork()) {
        short_k_backend.step();
    }
    const auto short_steps = DetailedCuteBackendTestProbe::backendStep(short_k_backend);

    DetailedCuteBackend long_k_backend;
    long_k_backend.matrixState().write(
        MatrixBankKind::A, 1, filledTensor(4, 64, MatrixElemType::Int8, 1));
    long_k_backend.matrixState().write(
        MatrixBankKind::B, 2, filledTensor(64, 128, MatrixElemType::Int8, 1));
    long_k_backend.matrixState().write(
        MatrixBankKind::C, 3, filledTensor(4, 128, MatrixElemType::Int32, 0));
    long_k_backend.submit(CuteRequest::makeMma(1, 3, 1, 2, 4, 128, 64));

    while (long_k_backend.hasWork()) {
        long_k_backend.step();
    }
    const auto long_steps = DetailedCuteBackendTestProbe::backendStep(long_k_backend);

    EXPECT_GT(long_steps, short_steps);
}

TEST(DetailedCuteBackend, ComputeLatencyScalesWithDatatypeWidth)
{
    DetailedCuteBackend int8_backend;
    int8_backend.matrixState().write(
        MatrixBankKind::A, 1, filledTensor(4, 32, MatrixElemType::Int8, 1));
    int8_backend.matrixState().write(
        MatrixBankKind::B, 2, filledTensor(32, 128, MatrixElemType::Int8, 1));
    int8_backend.matrixState().write(
        MatrixBankKind::C, 3, filledTensor(4, 128, MatrixElemType::Int32, 0));
    auto int8_mma = CuteRequest::makeMma(1, 3, 1, 2, 4, 128, 32);
    int8_mma.mma.types1 = 0x4;
    int8_mma.mma.types2 = 0x4;
    int8_mma.mma.typed = 0x2;
    int8_backend.submit(int8_mma);

    while (int8_backend.hasWork()) {
        int8_backend.step();
    }
    const auto int8_steps = DetailedCuteBackendTestProbe::backendStep(int8_backend);

    DetailedCuteBackend int16_backend;
    int16_backend.matrixState().write(
        MatrixBankKind::A, 1, filledTensor(4, 32, MatrixElemType::Int16, 1));
    int16_backend.matrixState().write(
        MatrixBankKind::B, 2, filledTensor(32, 128, MatrixElemType::Int16, 1));
    int16_backend.matrixState().write(
        MatrixBankKind::C, 3, filledTensor(4, 128, MatrixElemType::Int32, 0));
    auto int16_mma = CuteRequest::makeMma(1, 3, 1, 2, 4, 128, 32);
    int16_mma.mma.types1 = 0x1;
    int16_mma.mma.types2 = 0x1;
    int16_mma.mma.typed = 0x2;
    int16_backend.submit(int16_mma);

    while (int16_backend.hasWork()) {
        int16_backend.step();
    }
    const auto int16_steps = DetailedCuteBackendTestProbe::backendStep(int16_backend);

    EXPECT_GT(int16_steps, int8_steps);
}

TEST(DetailedCuteBackend, MteTimingReportsPerCycleBandwidthAndAcceptedBeats)
{
    DetailedCuteBackend backend;
    auto mma = CuteRequest::makeMma(1, 3, 1, 2, 128, 128, 64);
    mma.mma.types1 = 0x4;
    mma.mma.types2 = 0x4;
    mma.mma.typed = 0x2;

    const auto timing =
        DetailedCuteBackendTestProbe::computeMteTiming(backend, mma.mma);

    EXPECT_TRUE(timing.supported);
    EXPECT_EQ(timing.tensorMn, 128U);
    EXPECT_EQ(timing.tensorK, 64U);
    EXPECT_EQ(timing.matrixMn, 8U);
    EXPECT_EQ(timing.reduceWidthBytes, 32U);
    EXPECT_EQ(timing.resultWidthBytes, 4U);
    EXPECT_EQ(timing.aBytesPerBeat, 256U);
    EXPECT_EQ(timing.bBytesPerBeat, 256U);
    EXPECT_EQ(timing.cBytesPerBeat, 256U);
    EXPECT_EQ(timing.dBytesPerBeat, 256U);
    EXPECT_EQ(timing.aBytesPerBeat * 8, 2048U);
    EXPECT_EQ(timing.bBytesPerBeat * 8, 2048U);
    EXPECT_EQ(timing.cBytesPerBeat * 8, 2048U);
    EXPECT_EQ(timing.dBytesPerBeat * 8, 2048U);
    EXPECT_EQ(timing.acceptedInputBeats, 512U);
    EXPECT_EQ(timing.adcReadCycles, 1U);
    EXPECT_EQ(timing.bdcReadCycles, 1U);
    EXPECT_EQ(timing.mteAcceptedInputBeats, 512U);
    EXPECT_EQ(timing.fReduceTailCycles, 6U);
    EXPECT_EQ(timing.cdcWriteCycles, 1U);
    EXPECT_EQ(timing.terminalHandshakeCycles, 1U);
    EXPECT_GT(timing.totalCompletionCycles, timing.acceptedInputBeats);
}

TEST(DetailedCuteBackend, MteTimingFollowsActiveRtlControllerShape)
{
    DetailedCuteBackend backend;

    auto full = CuteRequest::makeMma(1, 3, 1, 2, 128, 128, 64);
    full.mma.types1 = 0x4;
    full.mma.types2 = 0x4;
    full.mma.typed = 0x2;
    EXPECT_EQ(
        DetailedCuteBackendTestProbe::computeMteTiming(backend, full.mma)
            .acceptedInputBeats,
        512U);

    auto partial_m = CuteRequest::makeMma(2, 3, 1, 2, 4, 128, 32);
    partial_m.mma.types1 = 0x4;
    partial_m.mma.types2 = 0x4;
    partial_m.mma.typed = 0x2;
    const auto partial_m_timing =
        DetailedCuteBackendTestProbe::computeMteTiming(
            backend, partial_m.mma);
    EXPECT_TRUE(partial_m_timing.supported);
    EXPECT_EQ(partial_m_timing.acceptedInputBeats, 16U);

    auto partial_n = CuteRequest::makeMma(3, 3, 1, 2, 64, 64, 32);
    partial_n.mma.types1 = 0x4;
    partial_n.mma.types2 = 0x4;
    partial_n.mma.typed = 0x2;
    EXPECT_FALSE(
        DetailedCuteBackendTestProbe::computeMteTiming(
            backend, partial_n.mma)
            .supported);

    auto e16 = CuteRequest::makeMma(4, 3, 1, 2, 128, 128, 64);
    e16.mma.types1 = 0x1;
    e16.mma.types2 = 0x1;
    e16.mma.typed = 0x2;
    EXPECT_EQ(
        DetailedCuteBackendTestProbe::computeMteTiming(backend, e16.mma)
            .acceptedInputBeats,
        1024U);

    auto e32 = CuteRequest::makeMma(5, 3, 1, 2, 128, 128, 64);
    e32.mma.types1 = 0x2;
    e32.mma.types2 = 0x2;
    e32.mma.typed = 0x2;
    EXPECT_EQ(
        DetailedCuteBackendTestProbe::computeMteTiming(backend, e32.mma)
            .acceptedInputBeats,
        2048U);

    auto e4 = CuteRequest::makeMma(6, 3, 1, 2, 128, 128, 64);
    e4.mma.types1 = 0x3;
    e4.mma.types2 = 0x3;
    e4.mma.typed = 0x2;
    EXPECT_EQ(
        DetailedCuteBackendTestProbe::computeMteTiming(backend, e4.mma)
            .acceptedInputBeats,
        256U);
}

TEST(DetailedCuteBackend, MteTotalCompletionCyclesMatchesComputeTerminalBoundary)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        filledTensor(4, 32, MatrixElemType::Int8, 1));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        filledTensor(32, 128, MatrixElemType::Int8, 1));
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        filledTensor(4, 128, MatrixElemType::Int32, 0));

    auto mma = CuteRequest::makeMma(1, 3, 1, 2, 4, 128, 32);
    mma.mma.types1 = 0x4;
    mma.mma.types2 = 0x4;
    mma.mma.typed = 0x2;
    const auto timing =
        DetailedCuteBackendTestProbe::computeMteTiming(backend, mma.mma);
    ASSERT_TRUE(timing.supported);

    backend.submit(mma);

    while (!DetailedCuteBackendTestProbe::computeUnitBusy(
        backend, DetailedCuteBackend::ComputeUnitKind::CDC)) {
        backend.step();
        EXPECT_FALSE(backend.hasCompletion());
        EXPECT_EQ(
            backend.matrixState().read(MatrixBankKind::C, 3).elements[0],
            0);
    }

    backend.step();
    EXPECT_FALSE(backend.hasCompletion());
    EXPECT_EQ(
        backend.matrixState().read(MatrixBankKind::C, 3).elements[0],
        32);

    while (!backend.hasCompletion()) {
        backend.step();
    }

    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().status, CuteCompletionStatus::Success);
    EXPECT_EQ(backend.traceCounters().lastMicrotaskLatency,
              timing.totalCompletionCycles);
}

TEST(DetailedCuteBackend, MteRejectsMatricesLargerThanFixedShape)
{
    DetailedCuteBackend backend;

    auto too_many_m = CuteRequest::makeMma(1, 3, 1, 2, 129, 128, 64);
    EXPECT_FALSE(DetailedCuteBackendTestProbe::computeMteTiming(
                     backend, too_many_m.mma)
                     .supported);

    auto too_many_n = CuteRequest::makeMma(2, 3, 1, 2, 128, 129, 64);
    EXPECT_FALSE(DetailedCuteBackendTestProbe::computeMteTiming(
                     backend, too_many_n.mma)
                     .supported);

    auto too_many_k = CuteRequest::makeMma(3, 3, 1, 2, 128, 128, 65);
    EXPECT_FALSE(DetailedCuteBackendTestProbe::computeMteTiming(
                     backend, too_many_k.mma)
                     .supported);
}

TEST(DetailedCuteBackend, LoadFillUsesSharedMemoryBudget)
{
    auto memory = std::make_unique<SparseMatrixMemoryAdapter>();
    auto *memory_ptr = memory.get();
    memory_ptr->writeElement(0x1000, 1);
    memory_ptr->writeElement(0x1001, 2);
    memory_ptr->writeElement(0x1004, 3);
    memory_ptr->writeElement(0x1005, 4);
    memory_ptr->writeElement(0x2000, 5);
    memory_ptr->writeElement(0x2001, 6);
    memory_ptr->writeElement(0x2004, 7);
    memory_ptr->writeElement(0x2005, 8);

    DetailedCuteBackend backend(std::move(memory));

    CuteRequest load_a;
    load_a.seq = 1;
    load_a.kind = CuteRequestKind::Lsu;
    load_a.lsu.ms = 0;
    load_a.lsu.isStore = false;
    load_a.lsu.isA = true;
    load_a.lsu.baseAddr = 0x1000;
    load_a.lsu.stride = 4;
    load_a.lsu.row = 2;
    load_a.lsu.column = 2;
    load_a.lsu.elemType = MatrixElemType::Int8;

    CuteRequest load_b;
    load_b.seq = 2;
    load_b.kind = CuteRequestKind::Lsu;
    load_b.lsu.ms = 1;
    load_b.lsu.isStore = false;
    load_b.lsu.isB = true;
    load_b.lsu.baseAddr = 0x2000;
    load_b.lsu.stride = 4;
    load_b.lsu.row = 2;
    load_b.lsu.column = 2;
    load_b.lsu.elemType = MatrixElemType::Int8;

    backend.submit(load_a);
    backend.submit(load_b);

    backend.step();
    backend.step();
    backend.step();

    EXPECT_FALSE(backend.matrixState().allocated(MatrixBankKind::B, 1));

    backend.step();
    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 1U);
    EXPECT_FALSE(backend.matrixState().allocated(MatrixBankKind::B, 1));

    backend.step();
    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 2U);
    EXPECT_TRUE(backend.matrixState().allocated(MatrixBankKind::B, 1));
}

TEST(DetailedCuteBackend, UnsupportedFpMmaReturnsUnsupportedCompletion)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        MatrixTensor{2, 2, MatrixElemType::Fp16, {0x3c00, 0x4000, 0x4200, 0x4400}});
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        MatrixTensor{2, 2, MatrixElemType::Fp16, {0x3c00, 0x4000, 0x4200, 0x4400}});
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        MatrixTensor{2, 2, MatrixElemType::Fp16, {0x3c00, 0x4000, 0x4200, 0x4400}});

    auto mma = CuteRequest::makeMma(1, 3, 1, 2, 2, 2, 2);
    mma.mma.isFp = true;
    mma.mma.lhsElemType = MatrixElemType::Fp16;
    mma.mma.rhsElemType = MatrixElemType::Fp16;
    mma.mma.dstElemType = MatrixElemType::Fp16;
    backend.submit(mma);

    while (backend.hasWork()) {
        backend.step();
    }

    ASSERT_TRUE(backend.hasCompletion());
    auto completion = backend.popCompletion();
    EXPECT_EQ(completion.status, CuteCompletionStatus::Unsupported);
    auto c = backend.matrixState().read(MatrixBankKind::C, 3);
    EXPECT_EQ(c.elemType, MatrixElemType::Fp16);
    EXPECT_EQ(c.elements[0], 0x3c00);
    EXPECT_EQ(c.elements[3], 0x4400);
}

TEST(DetailedCuteBackend, Fp16MmaSucceedsWithInt32Accumulator)
{
    DetailedCuteBackend backend;

    backend.matrixState().write(
        MatrixBankKind::A, 1,
        paddedTopLeftTensor(
            2, 32, MatrixElemType::Fp16, 2, 2,
            {0x3c00, 0x4000, 0x4200, 0x4400}));
    backend.matrixState().write(
        MatrixBankKind::B, 2,
        paddedTopLeftTensor(
            32, 128, MatrixElemType::Fp16, 2, 2,
            {0x3c00, 0x4000, 0x4200, 0x4400}));
    backend.matrixState().write(
        MatrixBankKind::C, 3,
        filledTensor(2, 128, MatrixElemType::Int32, 0));

    auto mma = CuteRequest::makeMma(1, 3, 1, 2, 2, 128, 32);
    mma.mma.isFp = true;
    mma.mma.types1 = 0x1;
    mma.mma.types2 = 0x1;
    mma.mma.typed = 0x2;
    mma.mma.lhsElemType = MatrixElemType::Fp16;
    mma.mma.rhsElemType = MatrixElemType::Fp16;
    mma.mma.dstElemType = MatrixElemType::Int32;
    backend.submit(mma);

    backend.step();
    backend.step();
    backend.step();
    backend.step();

    EXPECT_FALSE(backend.hasCompletion());
    while (backend.hasWork()) {
        backend.step();
    }

    ASSERT_TRUE(backend.hasCompletion());
    auto completion = backend.popCompletion();
    EXPECT_EQ(completion.status, CuteCompletionStatus::Success);
    const auto &c = backend.matrixState().read(MatrixBankKind::C, 3);
    EXPECT_EQ(c.elemType, MatrixElemType::Int32);
    expectTopLeft2x2(
        c, static_cast<int64_t>(0x40e00000u),
        static_cast<int64_t>(0x41200000u),
        static_cast<int64_t>(0x41700000u),
        static_cast<int64_t>(0x41b00000u));
}

TEST(DetailedCuteBackend, Fp16LsuKeepsElemTypeAndRawBits)
{
    auto memory = std::make_unique<SparseMatrixMemoryAdapter>();
    auto *memory_ptr = memory.get();
    memory_ptr->writeElement(0x1000, 0x3c00);
    memory_ptr->writeElement(0x1002, 0x4000);
    memory_ptr->writeElement(0x1004, 0x4200);
    memory_ptr->writeElement(0x1006, 0x4400);

    DetailedCuteBackend backend(std::move(memory));

    CuteRequest load_a;
    load_a.seq = 1;
    load_a.kind = CuteRequestKind::Lsu;
    load_a.lsu.ms = 0;
    load_a.lsu.isStore = false;
    load_a.lsu.isA = true;
    load_a.lsu.baseAddr = 0x1000;
    load_a.lsu.stride = 4;
    load_a.lsu.row = 2;
    load_a.lsu.column = 2;
    load_a.lsu.elemType = MatrixElemType::Fp16;

    backend.submit(load_a);
    while (backend.hasWork()) {
        backend.step();
    }

    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().status, CuteCompletionStatus::Success);
    const auto &tensor = backend.matrixState().read(MatrixBankKind::A, 0);
    EXPECT_EQ(tensor.elemType, MatrixElemType::Fp16);
    EXPECT_EQ(tensor.elements[0], 0x3c00);
    EXPECT_EQ(tensor.elements[3], 0x4400);
}

TEST(DetailedCuteBackend, MatrixRegFileTracksOwnerAndLastWriter)
{
    auto memory = std::make_unique<SparseMatrixMemoryAdapter>();
    auto *memory_ptr = memory.get();
    memory_ptr->writeElement(0x1000, 1);
    memory_ptr->writeElement(0x1001, 2);
    memory_ptr->writeElement(0x1004, 3);
    memory_ptr->writeElement(0x1005, 4);

    DetailedCuteBackend backend(std::move(memory));

    CuteRequest load_a;
    load_a.seq = 1;
    load_a.kind = CuteRequestKind::Lsu;
    load_a.lsu.ms = 1;
    load_a.lsu.isStore = false;
    load_a.lsu.isA = true;
    load_a.lsu.baseAddr = 0x1000;
    load_a.lsu.stride = 4;
    load_a.lsu.row = 2;
    load_a.lsu.column = 2;
    load_a.lsu.elemType = MatrixElemType::Int8;

    backend.submit(load_a);
    backend.step();

    EXPECT_FALSE(backend.matrixState().allocated(MatrixBankKind::A, 1));

    backend.step();
    EXPECT_FALSE(backend.matrixState().allocated(MatrixBankKind::A, 1));

    backend.step();
    EXPECT_FALSE(backend.matrixState().allocated(MatrixBankKind::A, 1));

    backend.step();
    EXPECT_TRUE(backend.matrixState().allocated(MatrixBankKind::A, 1));
}

TEST(DetailedCuteBackend, ZeroLoadDefersVisibleRegisterWriteUntilTaskFinish)
{
    DetailedCuteBackend backend;

    auto zero_a = CuteRequest::makeArithZero(
        1, MatrixBankKind::A, 1, 2, 2, MatrixElemType::Int8);

    backend.submit(zero_a);

    backend.step();
    EXPECT_FALSE(backend.matrixState().allocated(MatrixBankKind::A, 1));
    EXPECT_FALSE(backend.hasCompletion());

    backend.step();
    ASSERT_TRUE(backend.hasCompletion());
    EXPECT_EQ(backend.popCompletion().seq, 1U);
    EXPECT_TRUE(backend.matrixState().allocated(MatrixBankKind::A, 1));
}

TEST(DetailedCuteBackend, ScoreboardBlockCounterTracksSrcNotReady)
{
    auto memory = std::make_unique<SparseMatrixMemoryAdapter>();
    auto *memory_ptr = memory.get();
    memory_ptr->writeElement(0x1000, 1);
    memory_ptr->writeElement(0x1001, 2);
    memory_ptr->writeElement(0x1002, 3);
    memory_ptr->writeElement(0x1004, 4);
    memory_ptr->writeElement(0x1005, 5);
    memory_ptr->writeElement(0x1006, 6);

    DetailedCuteBackend backend(std::move(memory));

    CuteRequest load_a;
    load_a.seq = 1;
    load_a.kind = CuteRequestKind::Lsu;
    load_a.lsu.ms = 1;
    load_a.lsu.isStore = false;
    load_a.lsu.isA = true;
    load_a.lsu.baseAddr = 0x1000;
    load_a.lsu.stride = 4;
    load_a.lsu.row = 2;
    load_a.lsu.column = 3;
    load_a.lsu.elemType = MatrixElemType::Int8;

    auto mma = CuteRequest::makeMma(2, 3, 1, 2, 2, 2, 3);

    backend.submit(load_a);
    backend.submit(mma);

    backend.step();
    EXPECT_EQ(backend.traceCounters().fifoEnqueue, 2U);
    EXPECT_EQ(backend.traceCounters().fifoDequeue, 1U);
    EXPECT_EQ(backend.traceCounters().scoreboardBlock, 0U);

    backend.step();
    EXPECT_EQ(backend.traceCounters().scoreboardBlock, 1U);
    EXPECT_EQ(
        backend.traceCounters().scoreboardBlockReasons[
            static_cast<size_t>(DetailedCuteScoreboard::BlockReason::SrcNotReady)],
        1U);
}

TEST(DetailedCuteBackend, MicroTaskLifecycleCountersTrackIssueOccupyFinish)
{
    auto memory = std::make_unique<SparseMatrixMemoryAdapter>();
    auto *memory_ptr = memory.get();
    memory_ptr->writeElement(0x1000, 1);
    memory_ptr->writeElement(0x1001, 2);
    memory_ptr->writeElement(0x1004, 3);
    memory_ptr->writeElement(0x1005, 4);

    DetailedCuteBackend backend(std::move(memory));

    CuteRequest load_a;
    load_a.seq = 1;
    load_a.kind = CuteRequestKind::Lsu;
    load_a.lsu.ms = 1;
    load_a.lsu.isStore = false;
    load_a.lsu.isA = true;
    load_a.lsu.baseAddr = 0x1000;
    load_a.lsu.stride = 4;
    load_a.lsu.row = 2;
    load_a.lsu.column = 2;
    load_a.lsu.elemType = MatrixElemType::Int8;

    backend.submit(load_a);

    backend.step();
    EXPECT_EQ(backend.traceCounters().microtaskIssue, 1U);
    EXPECT_EQ(backend.traceCounters().microtaskOccupy, 0U);
    EXPECT_EQ(backend.traceCounters().microtaskFinish, 0U);

    backend.step();
    EXPECT_EQ(backend.traceCounters().microtaskOccupy, 1U);
    EXPECT_EQ(backend.traceCounters().microtaskFinish, 0U);

    backend.step();
    EXPECT_EQ(backend.traceCounters().microtaskFinish, 0U);

    backend.step();
    EXPECT_EQ(backend.traceCounters().microtaskFinish, 1U);
    EXPECT_EQ(backend.traceCounters().lastMicrotaskLatency, 3U);
    EXPECT_EQ(
        backend.traceCounters().microtaskFinishesByKind[
            static_cast<size_t>(DetailedCuteBackend::MicroTaskKind::AML)],
        1U);
}

TEST(DetailedCuteBackend, EndToEndLoadMmaStoreReleaseSequence)
{
    auto memory = std::make_unique<SparseMatrixMemoryAdapter>();
    auto *memory_ptr = memory.get();
    memory_ptr->writeElement(0x1000, 1);
    memory_ptr->writeElement(0x1001, 2);
    memory_ptr->writeElement(0x1002, 3);
    memory_ptr->writeElement(0x1020, 4);
    memory_ptr->writeElement(0x1021, 5);
    memory_ptr->writeElement(0x1022, 6);

    memory_ptr->writeElement(0x2000, 7);
    memory_ptr->writeElement(0x2001, 8);
    memory_ptr->writeElement(0x2080, 9);
    memory_ptr->writeElement(0x2081, 10);
    memory_ptr->writeElement(0x2100, 11);
    memory_ptr->writeElement(0x2101, 12);

    DetailedCuteBackend backend(std::move(memory));

    CuteRequest load_a;
    load_a.seq = 1;
    load_a.kind = CuteRequestKind::Lsu;
    load_a.lsu.ms = 1;
    load_a.lsu.isStore = false;
    load_a.lsu.isA = true;
    load_a.lsu.baseAddr = 0x1000;
    load_a.lsu.stride = 32;
    load_a.lsu.row = 2;
    load_a.lsu.column = 32;
    load_a.lsu.elemType = MatrixElemType::Int8;

    CuteRequest load_b;
    load_b.seq = 2;
    load_b.kind = CuteRequestKind::Lsu;
    load_b.lsu.ms = 2;
    load_b.lsu.isStore = false;
    load_b.lsu.isB = true;
    load_b.lsu.baseAddr = 0x2000;
    load_b.lsu.stride = 128;
    load_b.lsu.row = 32;
    load_b.lsu.column = 128;
    load_b.lsu.elemType = MatrixElemType::Int8;

    auto zero_c = CuteRequest::makeArithZero(
        3, MatrixBankKind::C, 3, 2, 128, MatrixElemType::Int32);
    auto mma = CuteRequest::makeMma(4, 3, 1, 2, 2, 128, 32);

    CuteRequest store_c;
    store_c.seq = 5;
    store_c.kind = CuteRequestKind::Lsu;
    store_c.lsu.ms = 3;
    store_c.lsu.isStore = true;
    store_c.lsu.isAcc = true;
    store_c.lsu.baseAddr = 0x3000;
    store_c.lsu.stride = 512;
    store_c.lsu.row = 2;
    store_c.lsu.column = 128;
    store_c.lsu.elemType = MatrixElemType::Int32;

    auto release = CuteRequest::makeRelease(6, 7);

    backend.submit(load_a);
    backend.submit(load_b);
    backend.submit(zero_c);
    backend.submit(mma);
    backend.submit(store_c);
    backend.submit(release);

    while (backend.hasWork()) {
        backend.step();
    }

    std::vector<CuteCompletion> completions;
    while (backend.hasCompletion()) {
        completions.push_back(backend.popCompletion());
    }

    ASSERT_EQ(completions.size(), 6U);
    const std::array<uint64_t, 6> expected_seq = {1, 3, 2, 4, 5, 6};
    for (size_t i = 0; i < completions.size(); ++i) {
        EXPECT_EQ(completions[i].seq, expected_seq[i]);
        EXPECT_EQ(completions[i].status, CuteCompletionStatus::Success);
    }

    int64_t value = 0;
    ASSERT_TRUE(memory_ptr->readElement(0x3000, value));
    EXPECT_EQ(value, 58);
    ASSERT_TRUE(memory_ptr->readElement(0x3004, value));
    EXPECT_EQ(value, 64);
    ASSERT_TRUE(memory_ptr->readElement(0x3200, value));
    EXPECT_EQ(value, 139);
    ASSERT_TRUE(memory_ptr->readElement(0x3204, value));
    EXPECT_EQ(value, 154);

}

} // namespace matrix
} // namespace gem5
