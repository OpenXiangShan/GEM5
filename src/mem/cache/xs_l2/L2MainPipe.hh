#ifndef __MEM_CACHE_L2_MAIN_PIPE_HH__
#define __MEM_CACHE_L2_MAIN_PIPE_HH__

#include <cstdint>
#include <deque>
#include <unordered_map>

#include "base/types.hh"
#include "mem/cache/xs_l2/CacheWrapper.hh"
#include "mem/cache/xs_l2/PipelineResources.hh"
#include "mem/packet.hh"

namespace gem5
{

class L2CacheSlice;

// For task source
// Indicate the source of the task that is being processed by the pipeline.
enum TaskSource
{
    NoWhere,
    L1MSHR,
    L1WQ,
    L2PF,
    L3Snoop,
    L2MSHRGrant,
    L2MSHRRelease,
};

class L2MainPipe
{
  public:
    L2MainPipe(L2CacheSlice* owner, unsigned depth);

    /**
     * Advance the pipeline to the next cycle.
     * @param now The current cycle.
     */
    void advance(Cycles now);

    /**
     * Get the extra resources that are required for the task.
     * Eg. L1MSHR need DirWrite when hit in L2 cache.
     * @param pkt The packet to check.
     * @param source The task source.
     * @return The extra resources that are required for the task.
     */
    PipelineResources getExtraResources(PacketPtr pkt, TaskSource source) const;

    /**
     * Set Block mechanism in L2MainPipe.
     * If some requests are already in the pipeline and have not updated the directory,
     * then the request at S1 should be blocked if they have the same set address.
     * @param pkt The packet to check.
     * @param source The task source.
     * @return True if should block the packet, false otherwise.
     */
    bool setBlockByDir(PacketPtr pkt, TaskSource source) const;

    /**
     * Check if we should be blocked by MCP2 in the start stage.
     * With DataSram bank checking - requests to different banks won't stall each other.
     * @param resource The resource to check.
     * @param pkt The packet to check for bank conflicts.
     * @return True if the resource is available, false otherwise.
     */
    bool hasMCP2Stall(PipelineResources resource, PacketPtr pkt) const;

    /**
     * Check if we should be blocked by DirSram in the start stage.
     * @param resource The resource to check.
     * @param pkt The packet to check for bank conflicts.
     * @return True if the resource is available, false otherwise.
     */
    bool hasDirSramStall(PipelineResources resource, PacketPtr pkt) const;

    /**
     * Check if all pipeline resources are available for the task.
     * @param resource The resource to check.
     * @param pkt The packet to check for bank conflicts.
     * @return True if the resource is available, false otherwise.
     */
    bool isResourceAvailable(PipelineResources resource, PacketPtr pkt) const;

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
    inline PipelineResources getPipelineResources(PacketPtr pkt, TaskSource source) const;

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

    L2CacheSlice* owner;
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
