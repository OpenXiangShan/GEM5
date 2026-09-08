#ifndef __GEM5_O3_SMT_SCHED_HH__
#define __GEM5_O3_SMT_SCHED_HH__


#include <vector>

#include <boost/circular_buffer.hpp>

#include "base/types.hh"
#include "cpu/o3/limits.hh"

namespace gem5
{

namespace o3
{

class InstsCounter
{
    // can be placed in ftq、rob、lsq、iq, etc. to count the number of instructions
    uint64_t counter[MaxThreads];
  public:
    InstsCounter() {
        for (int i = 0; i < MaxThreads; ++i) {
            counter[i] = 0;
        }
    }

    uint64_t getCounter(ThreadID tid) { return counter[tid]; }
    void setCounter(ThreadID tid, uint64_t value) { counter[tid] = value; }
    void incCounter(ThreadID tid, uint64_t value = 1) { counter[tid] += value; }
    void decCounter(ThreadID tid, uint64_t value = 1) { counter[tid] -= value; }
};

class SMTScheduler
{
  protected:
    int numThreads;
  public:
    SMTScheduler(int numThreads) : numThreads(numThreads) {}
    virtual ~SMTScheduler() = default;
    virtual ThreadID getThread() = 0;
};


class ICountScheduler : public SMTScheduler
{
    // count of inst based smt shceduler
  protected:
    InstsCounter* counter;

  public:
    ICountScheduler(int numThreads, InstsCounter* counter) : SMTScheduler(numThreads), counter(counter) {}

    ThreadID getThread() override
    {
        // return the thread with the least number of instructions executed
        ThreadID selectedTid = 0;
        uint64_t minCount = counter->getCounter(0);
        for (ThreadID tid = 1; tid < numThreads; ++tid) {
            uint64_t count = counter->getCounter(tid);
            if (count < minCount) {
                minCount = count;
                selectedTid = tid;
            }
        }
        return selectedTid;
    }
};

class DelayedICountScheduler : public ICountScheduler
{
    // delayed count of inst based smt shceduler, which can be used to implement round-robin scheduling
  protected:
    boost::circular_buffer<ThreadID> timebuffer;

  public:
    DelayedICountScheduler(int numThreads, InstsCounter* counter, int delay) : ICountScheduler(numThreads, counter)
    {
        timebuffer.set_capacity(delay);
        for (int i = 0; i < delay; ++i) {
            timebuffer.push_back(i % numThreads);
        }
    }

    ThreadID getThread() override
    {
        // return the thread with the least number of instructions executed among the threads in the timebuffer
        ThreadID selectedTid = timebuffer.front();
        timebuffer.pop_front();
        timebuffer.push_back(ICountScheduler::getThread());
        // check whether candidate
        for (uint64_t i = 0; i < numThreads; i++) {
            ThreadID tid = (selectedTid + i) % numThreads;
            uint64_t count = counter->getCounter(tid);
            if (count < UINT64_MAX) {
                selectedTid = tid;
                break;
            }
        }
        return selectedTid;
    }
};


class MultiPrioritySched : public SMTScheduler
{
    // multi priority based smt scheduler
  private:
    std::vector<InstsCounter*> counter;

  public:
    // priority: higest -> lowest
    MultiPrioritySched(int numThreads, std::initializer_list<InstsCounter*> counters)
        : SMTScheduler(numThreads), counter(counters) {}

    ThreadID getThread() override
    {
        ThreadID selectedTid = 0;

        for (ThreadID tid = 1; tid < numThreads; ++tid) {
            for (size_t i = 0; i < counter.size(); ++i) {
                uint64_t candidateCount = counter[i]->getCounter(tid);
                uint64_t selectedCount = counter[i]->getCounter(selectedTid);

                if (candidateCount < selectedCount) {
                    selectedTid = tid;
                    break;
                }

                if (candidateCount > selectedCount) {
                    break;
                }
            }
        }

        return selectedTid;
    }
};

class RoundRobinScheduler : public SMTScheduler
{
    // count of inst based smt shceduler
  protected:
    InstsCounter* counter;
    uint64_t selectedTimes = 0;

  public:
    RoundRobinScheduler(int numThreads, InstsCounter* counter) : SMTScheduler(numThreads), counter(counter) {}

    ThreadID getThread() override
    {
        ThreadID selectedTid = 0;
        for (uint64_t i = 0; i < numThreads; i++) {
            ThreadID tid = (selectedTimes + i) % numThreads;
            uint64_t count = counter->getCounter(tid);
            if (count < UINT64_MAX - 1) {
                selectedTid = tid;
                break;
            }
        }
        selectedTimes++;
        return selectedTid;
    }
};


}}
#endif
