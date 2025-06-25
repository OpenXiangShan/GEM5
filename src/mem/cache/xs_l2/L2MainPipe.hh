#ifndef __MEM_CACHE_L2_MAIN_PIPE_HH__
#define __MEM_CACHE_L2_MAIN_PIPE_HH__

#include <deque>
#include <unordered_map>

#include "base/types.hh"
#include "mem/cache/xs_l2/PipelineResources.hh"
#include "mem/packet.hh"

namespace gem5
{

class L2CacheWrapper;

// For task source
// Indicate the source of the task that is being processed by the pipeline.
enum TaskSource
{
    NoWhere,
    L1MSHR,
    L1WQ,
    L3Snoop,
    L2MSHRGrant,
    L2MSHRRelease,
};

class L2MainPipe
{
  public:
    L2MainPipe(L2CacheWrapper* owner, unsigned depth);

    /**
     * Advance the pipeline to the next cycle.
     * @param now The current cycle.
     */
    void advance(Cycles now);

    /**
     * Check if a resource is available in the start stage.
     * @param resource The resource to check.
     * @return True if the resource is available, false otherwise.
     */
    bool isResourceAvailable(PipelineResources resource) const;

    /**
     * Check if a task is available in the start stage.
     * @param source The task source to check.
     * @return True if the task is available, false otherwise.
     */
    bool isTaskAvailable(TaskSource source) const;

    /**
     * Build a task in the start stage.
     * @param pkt The packet to build the task.
     * @param source The task source to build.
     */
    void buildTask(PacketPtr pkt, TaskSource source);

    /**
     * Send the MSHR grant packet from the pipeline.
     */
    void sendMSHRGrantPkt();

    /**
     * Check if the pipeline has work to do.
     * @return True if the pipeline has work to do, false otherwise.
     */
    bool hasWork() const;

  private:
    struct PipelineTask
    {
        TaskSource source;
        PacketPtr pkt;
        PipelineTask(TaskSource source, PacketPtr pkt) : source(source), pkt(pkt) {}
    };

    L2CacheWrapper* owner;
    std::deque<PipelineResources> scoreboardResources; // Bitmask of PipelineResources
    std::deque<PipelineTask> scoreboardTasks; // Bitmask of TaskSource
    std::unordered_map<TaskSource, PipelineResources> taskResourceMap;
    Cycles cur_cycle;

    /**
     * Advance the pipeline to the next cycle.
     */
    void advance();
};

} // namespace gem5

#endif // __MEM_CACHE_L2_MAIN_PIPE_HH__
