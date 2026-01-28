#include "cpu/pred/btb/fetch_target_queue.hh"

#include "fetch_target_queue.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

void FetchTargetQueue::consumeFetchTarget(unsigned ftq_id, unsigned fsq_id,
                                        unsigned fetched_inst_num)
{
    // Legacy interface: keep ftqId = fsqId - 1 for now.
    assert(ftq_id + 1 == fsq_id);
    assert(fsq_id == fetchHeadFtqId);
    assert(hasTargetEntry(fsq_id));
    getTarget(fsq_id).fetchInstNum = fetched_inst_num;
    fetchHeadFtqId++;
};

}
}
}
