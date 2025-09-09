#ifndef __CPU_PRED_BTB_CBP_SHADOW_CBP_TAGE_HH__
#define __CPU_PRED_BTB_CBP_SHADOW_CBP_TAGE_HH__

#include <cstdint>
#include <memory>

namespace gem5 {
namespace branch_prediction {
namespace btb_pred {
namespace cbp_pred {

// forward declaration to avoid pulling the heavy header into multiple TUs
class CBP2016_TAGE_SC_L;

class ShadowCBPTageAdapter
{
  public:
    ShadowCBPTageAdapter();
    ~ShadowCBPTageAdapter();

    bool predict(uint64_t seq_no, uint8_t piece, uint64_t pc);

    void update(uint64_t seq_no, uint8_t piece, uint64_t pc,
                bool resolveDir, bool predDir, uint64_t nextPC);

    void trackOtherInst(uint64_t pc, int brtype, bool pred_taken,
                        bool taken, uint64_t nextPC);

    // Speculation support: snapshot, speculative update, and finalize
    void snapshot(uint64_t seq_no);
    void speculativeUpdate(uint64_t seq_no, uint64_t pc, int brtype,
                           bool pred_taken, uint64_t nextPC);
    void finalizeUpdate(uint64_t seq_no, uint64_t pc, int brtype,
                        bool pred_taken, bool actual_taken, uint64_t nextPC);

    // Explicitly cleanup a seq if it will never be updated
    void cleanupSeq(uint64_t seq_no);

  private:
    std::unique_ptr<CBP2016_TAGE_SC_L> core;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cbp_pred
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_CBP_SHADOW_CBP_TAGE_HH__

