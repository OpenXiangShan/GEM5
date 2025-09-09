#include "cpu/pred/btb/cbp/shadow_cbp_tage.hh"
#include "cpu/pred/btb/cbp/cbp2016_tage_sc_l.h"

namespace gem5 {
namespace branch_prediction {
namespace btb_pred {
namespace cbp_pred {

ShadowCBPTageAdapter::ShadowCBPTageAdapter()
  : core(std::make_unique<CBP2016_TAGE_SC_L>())
{}

ShadowCBPTageAdapter::~ShadowCBPTageAdapter() = default;

bool ShadowCBPTageAdapter::predict(uint64_t seq_no, uint8_t piece, uint64_t pc)
{
    return core->predict(seq_no, piece, pc);
}

void ShadowCBPTageAdapter::update(uint64_t seq_no, uint8_t piece, uint64_t pc,
                                  bool resolveDir, bool predDir, uint64_t nextPC)
{
    core->update(seq_no, piece, pc, resolveDir, predDir, nextPC);
}

void ShadowCBPTageAdapter::trackOtherInst(uint64_t pc, int brtype, bool pred_taken,
                                          bool taken, uint64_t nextPC)
{
    core->TrackOtherInst(pc, brtype, pred_taken, taken, nextPC);
}

struct ShadowCBPTageAdapter::Impl {
    std::unordered_map<uint64_t, cbp_hist_t> snap;
    static constexpr size_t kMaxSnapshots = 8192; // hard cap to avoid OOM
};

void ShadowCBPTageAdapter::snapshot(uint64_t seq_no)
{
    if (!impl) impl = std::make_unique<Impl>();
    if (impl->snap.size() >= Impl::kMaxSnapshots) {
        // Drop oldest arbitrarily: erase begin()
        impl->snap.erase(impl->snap.begin());
    }
    impl->snap[seq_no] = core->active_hist;
}

void ShadowCBPTageAdapter::speculativeUpdate(uint64_t seq_no, uint64_t pc, int brtype,
                                             bool pred_taken, uint64_t nextPC)
{
    // advance history speculatively
    core->HistoryUpdate(pc, brtype, pred_taken, pred_taken, nextPC);
}

void ShadowCBPTageAdapter::finalizeUpdate(uint64_t seq_no, uint64_t pc, int brtype,
                                          bool pred_taken, bool actual_taken, uint64_t nextPC)
{
    if (impl && impl->snap.count(seq_no) && (pred_taken != actual_taken)) {
        core->active_hist = impl->snap[seq_no];
    }
    core->HistoryUpdate(pc, brtype, pred_taken, actual_taken, nextPC);
    if (impl) impl->snap.erase(seq_no);
}

void ShadowCBPTageAdapter::cleanupSeq(uint64_t seq_no)
{
    if (impl) impl->snap.erase(seq_no);
}

} // namespace cbp_pred
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

