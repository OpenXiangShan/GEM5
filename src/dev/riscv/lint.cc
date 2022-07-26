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
        case CLINT_MSIP:
            ret_val = msip;
            break;
        case CLINT_MTIMECMP:
            ret_val = mtimecmp;
            break;
    }
    DPRINTF(Lint, "read Lint offset:%#lx val:%lu\n", offset, ret_val);
    update_mtip();
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
            msip = write_val;
        } break;
        case CLINT_MTIMECMP: {
            write_val = pkt->getRaw<uint64_t>();
            mtimecmp = write_val;
            tryClearMtip();
        } break;
    }
    DPRINTF(Lint, "write Lint offset:%#lx val:%lu\n", offset, write_val);
    update_mtip();
    pkt->makeAtomicResponse();
    return pioDelay;
}

void
Lint::update_mtip(void)
{
    DPRINTF(Lint, "intr en: %i, mtime: %lu, time cmp: %lu\n", int_enable,
            mtime, mtimecmp);
    if (int_enable && mtime >= mtimecmp) {
        DPRINTF(Lint, "post mtip! time:%x cmp:%x\n", mtime, mtimecmp);
        for (int context_id = 0; context_id < numThreads; context_id++) {
            auto tc = sys->threads[context_id];
            tc->getCpuPtr()->postInterrupt(tc->threadId(), INT_TIMER_MACHINE,
                                           0);
        }
    }
}

void
Lint::update_time()
{
    mtime += 1;
    update_mtip();
    DPRINTF(Lint, "scheudle update time event at tick: %lu\n",
            curTick() + interval);
    this->reschedule(update_lint_event, curTick() + interval, true);
}

void
Lint::tryClearMtip()
{
    if (mtime < mtimecmp) {
        DPRINTF(Lint, "Clear mtip! time:%x cmp:%x\n", mtime, mtimecmp);
        for (int context_id = 0; context_id < numThreads; context_id++) {
            auto tc = sys->threads[context_id];
            tc->getCpuPtr()->clearInterrupt(tc->threadId(), INT_TIMER_MACHINE,
                                            0);
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
      msip(0),
      mtimecmp(999999),
      numThreads(p.num_threads),
      update_lint_event([this] { update_time(); }, "update clint time")
{
    interval = (Tick)(1 * SimClock::Float::us);
    DPRINTF(Lint, "scheudle update time event at tick: %lu\n",
            curTick() + interval);
    this->schedule(update_lint_event, curTick() + interval);
}
}
