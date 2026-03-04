// #ifndef __CPU_PRED_BTB_PERCEPTRON_BP_HH__
// #define __CPU_PRED_BTB_PERCEPTRON_BP_HH__

// #include <cstdint>
// #include <deque>
// #include <map>
// #include <utility>
// #include <vector>

// #include <unordered_map>
// #include "base/types.hh"
// #include "cpu/inst_seq.hh"
// #include "cpu/pred/btb/common.hh"
// #include "cpu/pred/btb/timed_base_pred.hh"

// namespace gem5 {
// namespace branch_prediction {
// namespace btb_pred {

// // 感知机分支预测器类
// class PerceptronBP : public TimedBaseBTBPredictor {
// public:
//     // 定义权重表中单个权重的结构（可以是整数，但通常用饱和计数器）
//     struct WeightEntry {
//         int weight; // 权重值，可以是8位有符号整数
//         WeightEntry() : weight(0) {}
//         WeightEntry(int w) : weight(w) {}
//     };

//     // 定义感知机预测结果
//     struct PerceptronPrediction {
//         bool taken; // 最终预测结果
//         int sum;    // 点积和值，用于置信度判断
//         unsigned index; // 权重表索引
//         std::vector<int> inputs; // 输入历史向量

//         PerceptronPrediction() : taken(false), sum(0), index(0) {}
//         PerceptronPrediction(bool taken, int sum, unsigned index,
//                             const std::vector<int>& inputs)
//             : taken(taken), sum(sum), index(index), inputs(inputs) {}
//     };

//     // 构造函数（生产环境）
//     PerceptronBP(const Params& p);

//     // 析构函数
//     ~PerceptronBP();

//     // 重写TimedBaseBTBPredictor的接口函数
//     void tickStart() override;
//     void tick() override;
//     void dryRunCycle(Addr startPC) override;
//     void putPCHistory(Addr startPC, const boost::dynamic_bitset<> &history,
//                       std::vector<FullBTBPrediction> &stagePreds) override;
//     std::shared_ptr<void> getPredictionMeta() override;
//     void specUpdatePHist(const boost::dynamic_bitset<> &history,
//                          FullBTBPrediction &pred) override;
//     void recoverPHist(const boost::dynamic_bitset<> &history,
//                       const FetchTarget &entry, int shamt, bool cond_taken) override;
//     void update(const FetchTarget &stream) override;
//     bool canResolveUpdate(const FetchTarget &stream) override;
//     void doResolveUpdate(const FetchTarget &stream) override;
//     void commitBranch(const FetchTarget &stream, const DynInstPtr &inst) override;

//     // 设置跟踪信息
//     void setTrace() override;

//     // 检查折叠历史（用于调试）
//     void checkFoldedHist(const boost::dynamic_bitset<> &history, const char *when);

// private:
//     // 辅助函数：根据PC获取权重表索引
//     unsigned getWeightIndex(Addr pc) const;

//     // 辅助函数：获取分支历史向量（双极性表示）
//     std::vector<int> getBranchHistoryVector() const;

//     // 辅助函数：计算点积和
//     int calculateSum(const std::vector<int>& inputs,
//                     const std::vector<WeightEntry>& weights) const;

//     // 辅助函数：更新全局分支历史寄存器
//     void updateGBHR(bool taken);

//     // 辅助函数：更新权重表（用于学习）
//     void updateWeightTable(unsigned index, const std::vector<int>& inputs,
//                           int actual_outcome);

//     // 辅助函数：饱和算术（防止权重溢出）
//     int saturateWeight(int value) const;

//     // 预测单个BTB条目
//     PerceptronPrediction generateSinglePrediction(const BTBEntry &btb_entry,
//                                                 const Addr &startPC);

//     // 准备更新的BTB条目
//     std::vector<BTBEntry> prepareUpdateEntries(const FetchTarget &stream);

//     // 主要的预测逻辑
//     void lookupHelper(const Addr &startPC, const std::vector<BTBEntry> &btbEntries,
//                      std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs,
//                      CondTakens& results);

//     // 权重表：二维向量，[分支PC索引][历史位]
//     std::vector<std::vector<WeightEntry>> weightTable;

//     // 全局分支历史寄存器 (GBHR)
//     std::vector<int> gbhr; // 双极性表示：1 = Taken, -1 = Not Taken

//     // 全局分支历史长度
//     unsigned historyLength;

//     // 每个权重的位宽
//     unsigned weightBits;

//     // 预测置信度阈值
//     int threshold;

//     // 权重表大小
//     unsigned tableSize;

//     // 统计信息
//     struct Stats {
//         // 添加必要的统计项
//         // ...
//     } stats;
// };

// } // namespace btb_pred
// } // namespace branch_prediction
// } // namespace gem5

// #endif // __CPU_PRED_BTB_PERCEPTRON_BP_HH__