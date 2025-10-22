#ifndef __MEM_CACHE_L3_MAIN_PIPE_HH__
#define __MEM_CACHE_L3_MAIN_PIPE_HH__

#include <cstdint>
#include <deque>
#include <unordered_map>

#include "base/types.hh"
#include "mem/cache/xs_l3/BaseCacheWrapper.hh"
#include "mem/cache/xs_l3/L3PipelineResources.hh"
#include "mem/packet.hh"

namespace gem5
{

class L3CacheSlice;

// For task source
// Indicate the source of the task that is being processed by the pipeline.
enum TaskSource
{
    NoWhere,
    L2MSHR, // L2 MSHR (Miss Status Holding Register) request(Read)
    L2WQ, // L2 Write Queue request
    L3PF,
    //L3Snoop, // L3 Snoop request (not needed, MEM won't send snoop requests upwards)
    L3MSHRGrant,
    L3MSHRRelease,
};

class L3MainPipe
{
  public:
    L3MainPipe(L3CacheSlice* owner, unsigned depth);

    /**
     * Advance the pipeline to the next cycle.
     * @param now The current cycle.
     */
    void advance(Cycles now);

    /**
     * Get the extra resources that are required for the task.
     * Eg. L2MSHR need DirWrite when hit in L3 cache.
     * @param pkt The packet to check.
     * @param source The task source.
     * @return The extra resources that are required for the task.
     */
    L3PipelineResources getExtraResources(PacketPtr pkt, TaskSource source) const;

    /**
     * Set Block mechanism in L3MainPipe.
     * If some requests are already in the pipeline and have not updated the directory,
     * then the request at S1 should be blocked if they have the same set address.
     * @param pkt The packet to check.
     * @param source The task source.
     * @return True if should block the packet, false otherwise.
     */
    bool setBlockByDir(PacketPtr pkt, TaskSource source) const;

    /**
     * Check if we should be blocked by MCP2 in the start stage.
     * @param resource The resource to check.
     * @return True if the resource is available, false otherwise.
     */
    bool hasMCP2Stall(L3PipelineResources resource) const;

    /**
     * Check if we should be blocked by DirSram in the start stage.
     * @param resource The resource to check.
     * @return True if the resource is available, false otherwise.
     */
    bool hasDirSramStall(L3PipelineResources resource) const;

    /**
     * Check if a resource is available in the start stage.
     * @param resource The resource to check.
     * @return True if the resource is available, false otherwise.
     */
    bool isResourceAvailable(L3PipelineResources resource) const;

    /**
     * Check if a task is available in the start stage.
     * @param pkt The packet to check.
     * @param source The task source to check.
     * @return True if the task is available, false otherwise.
     */
    bool isTaskAvailable(PacketPtr pkt, TaskSource source) const;

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

    /**
     * Get the stage of directory write.
     * @return The stage of directory write.
     */
    inline uint64_t getDirWriteStage() const;

    /**
     * Get the pipeline resources for a task.
     * @param pkt The packet to get the resources.
     * @param source The task source.
     * @return The pipeline resources for the task.
     */
    inline L3PipelineResources getL3PipelineResources(PacketPtr pkt, TaskSource source) const;

  private:
    struct PipelineTask
    {
        TaskSource source;
        PacketPtr pkt;
        Addr addr;
        PipelineTask(TaskSource source, PacketPtr pkt) : source(source), pkt(pkt), addr(0) {
          if (pkt) {
            addr = pkt->getAddr();
          }
        }
    };

    L3CacheSlice* owner;
    std::deque<L3PipelineResources> scoreboardResources; // Bitmask of L3PipelineResources
    std::deque<PipelineTask> scoreboardTasks; // Bitmask of TaskSource
    std::unordered_map<TaskSource, L3PipelineResources> taskResourceMap;
    Cycles cur_cycle;

    /**
     * Advance the pipeline to the next cycle.
     */
    void advance();
};

} // namespace gem5

#endif // __MEM_CACHE_L3_MAIN_PIPE_HH_