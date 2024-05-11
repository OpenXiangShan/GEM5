//
// Created by zyy on 2020/11/24.
//

#include "base/trace.hh"
#include "cpu/base.hh"
#include "debug/Lint.hh"
#include "lint.hh"
#include "mem/packet_access.hh"
#include "sim/system.hh"

namespace gem5 {

Tick
Lint::read(PacketPtr pkt)
{
    // static_assert(std::is_constructible_v<Lint, const LintParams &>);
    assert(pkt->getAddr() >= pioAddr && pkt->getAddr() < pioAddr + pioSize);
    assert(pkt->getSize() > 0 && pkt->getSize() <= 8);

    Addr offset = pkt->getAddr() - pioAddr;
    uint64_t ret_val = 0;
    switch (offset) {
        case CLINT_FREQ:
            ret_val = freq;
            break;
        case CLINT_INC:
            ret_val = inc;
            break;
        case CLINT_MTIME:
            ret_val = mtime;
            break;
        case CLINT_MSIP ... CLINT_MSIP + 4 * MaxThreads:
            ret_val = msip[(offset - CLINT_MSIP) / 4];
            break;
        case CLINT_MTIMECMP ... CLINT_MTIMECMP + 8 * MaxThreads:
            ret_val = mtimecmp[(offset - CLINT_MTIMECMP) / 8];
            break;
        default: panic("Invalid Lint register offset %#lx\n", offset);
    }
    DPRINTF(Lint, "Context %i read Lint offset:%#lx val:%lu\n", pkt->req->contextId(), offset, ret_val);
    pkt->setLE(ret_val);
    pkt->makeAtomicResponse();
    return pioDelay;
}

Tick
Lint::write(PacketPtr pkt)
{
    DPRINTF(Lint, "Lint addr:%lx, size: %lu\n", pkt->getAddr(),
            pkt->getSize());
    assert(pkt->getAddr() >= pioAddr && pkt->getAddr() < pioAddr + pioSize);
    assert(pkt->getSize() > 0 && pkt->getSize() <= 8);

    Addr offset = pkt->getAddr() - pioAddr;
    DPRINTF(Lint, "write Lint offset:%#lx\n", offset);
    uint64_t write_val;
    switch (offset) {
        case CLINT_FREQ: {
            write_val = pkt->getRaw<uint64_t>();
            freq = write_val;
        } break;
        case CLINT_INC: {
            write_val = pkt->getRaw<uint64_t>();
            inc = write_val;
        } break;
        case CLINT_MTIME: {
            write_val = pkt->getRaw<uint64_t>();
            mtime = write_val;
        } break;
        case CLINT_MSIP: {
            write_val = pkt->getRaw<uint32_t>();
            msip[(offset - CLINT_MSIP) / 4] = write_val;
        } break;
        case CLINT_MTIMECMP ... CLINT_MTIMECMP + 16: {
            write_val = pkt->getRaw<uint64_t>();
            mtimecmp[(offset - CLINT_MTIMECMP) / 8] = write_val;
            tryClearMtip();
        } break;
        default: panic("Invalid Lint register offset %#lx\n", offset);
    }
    DPRINTF(Lint, "Context %i write Lint offset:%#lx val:%lu\n", pkt->req->contextId(), offset, write_val);
    pkt->makeAtomicResponse();
    return pioDelay;
}

void
Lint::tryPostInterrupt(uint64_t old_time)
{
    DPRINTF(Lint, "intr en: %i, mtime: %lu\n", int_enable, mtime);
    for (int context_id = 0; context_id < numThreads; context_id++) {
        if (int_enable && old_time < mtimecmp[context_id] && mtime == mtimecmp[context_id]) {
            auto tc = sys->threads[context_id];
            tc->getCpuPtr()->postInterrupt(tc->threadId(), INT_TIMER_MACHINE, 0);
        }
    }
}

void
Lint::update_time()
{
    mtime += 1;
    // tryPostInterrupt();
    DPRINTF(Lint, "scheudle update time event at tick: %lu\n",
            curTick() + interval);
    this->reschedule(update_lint_event, curTick() + interval, true);
}

void
Lint::tryClearMtip()
{
    DPRINTF(Lint, "Try clear mtip, time:%lu\n", mtime);
    for (int context_id = 0; context_id < numThreads; context_id++) {
        if (mtime < mtimecmp[context_id]) {
            DPRINTF(Lint, "Clear mtip for context %i, time: %lu, cmp: %lu\n", context_id, mtime, mtimecmp[context_id]);
            auto tc = sys->threads[context_id];
            tc->getCpuPtr()->clearInterrupt(tc->threadId(), INT_TIMER_MACHINE, 0);
        }
    }
}

Lint::Lint(const LintParams &p)
    : BasicPioDevice(p, p.pio_size),
      lint_id(p.lint_id),
      int_enable(p.int_enable),
      freq(0),
      inc(0),
      mtime(0),
      msip(p.num_threads, 0),
      mtimecmp(p.num_threads, 12345678),
      numThreads(p.num_threads),
      update_lint_event([this] { update_time(); }, "update clint time")
{
    interval = (Tick)(1 * sim_clock::as_float::us);
    DPRINTF(Lint, "schedule update time event at tick: %lu\n",
            curTick() + interval);
    this->schedule(update_lint_event, curTick() + interval);
}
}
