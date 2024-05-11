//
// Created by zyy on 2020/11/24.
//

#ifndef GEM5_LINT_HH
#define GEM5_LINT_HH


#include "debug/Lint.hh"
#include "dev/io_device.hh"
#include "params/Lint.hh"

#define CLINT_MSIP     0x0000
#define CLINT_MTIMECMP 0x4000
#define CLINT_FREQ     0x8000
#define CLINT_INC      0x8008
#define CLINT_MTIME    0xBFF8
#define INT_TIMER_MACHINE 7

namespace gem5 {

const int MaxThreads=1024;

class Lint: public BasicPioDevice
{
  private:
    Tick interval;
    int lint_id;
    bool int_enable;
    uint64_t freq, inc, mtime;
    std::vector<uint32_t> msip;
    std::vector<uint64_t> mtimecmp;
    int numThreads;
    EventFunctionWrapper update_lint_event;

  public:
    typedef LintParams Params;
    Lint(const Params &p);

    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
    void tryPostInterrupt(uint64_t old_time);
    void update_time(void);
    void tryClearMtip(void);
};

}

#endif //GEM5_LINT_HH
