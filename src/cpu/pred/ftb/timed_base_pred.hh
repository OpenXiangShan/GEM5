#ifndef __CPU_PRED_FTB_TIMED_BASE_PRED_HH__
#define __CPU_PRED_FTB_TIMED_BASE_PRED_HH__


#include <boost/dynamic_bitset.hpp>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/pred/ftb/stream_struct.hh"
#include "sim/sim_object.hh"
#include "params/TimedBaseFTBPredictor.hh"

namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

using DynInstPtr = o3::DynInstPtr;

class TimedBaseFTBPredictor: public SimObject
{
    public:

    typedef TimedBaseFTBPredictorParams Params;

    TimedBaseFTBPredictor(const Params &params);

    virtual void tickStart() {}
    virtual void tick() {}
    // make predictions, record in stage preds 做预测，记录在stagePreds中
    virtual void putPCHistory(Addr startAddr,
                              const boost::dynamic_bitset<> &history,
                              std::vector<FullFTBPrediction> &stagePreds) {}

    virtual std::shared_ptr<void> getPredictionMeta() { return nullptr; }   // 获取预测元数据
    // meta 每个分支一个，相当于checkpoints, 存储预测前的状态，预测错误时候回滚，指令提交时候验证

    virtual void specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) {}  // 推测更新历史, 只有uRAS实现
    virtual void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) {}  // squash后恢复历史, tage/RAS实现，并更新s0history
    virtual void update(const FetchStream &entry) {}  // 用commit stream更新预测器内容, 准确的！
    unsigned getDelay() { return numDelay; }  // 获取预测结果的延迟
    // do some statistics on a per-branch and per-predictor basis
    virtual void commitBranch(const FetchStream &entry, const DynInstPtr &inst) {}  // commit阶段统计分支数据

    int componentIdx;
    int getComponentIdx() { return componentIdx; }
    void setComponentIdx(int idx) { componentIdx = idx; }

    bool hasDB {false};
    std::string dbName;
    bool enableDB {false};
    void setDB(DataBase *db) {
        _db = db;
    }
    virtual void setTrace() {}
    DataBase *_db;

  private:
    unsigned int numDelay; // 气泡数, 预测结果的延迟
};

} // namespace ftb_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_FTB_TIMED_BASE_PRED_HH__
