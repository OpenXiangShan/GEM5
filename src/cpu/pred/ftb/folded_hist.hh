#ifndef __CPU_PRED_FTB_FOLDED_HIST_HH__
#define __CPU_PRED_FTB_FOLDED_HIST_HH__

#include <boost/dynamic_bitset.hpp>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "debug/FTBFoldedHist.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred {

class FoldedHist {
    private:
        int histLen;    // 历史长度
        int foldedLen;  // 折叠后历史长度
        int maxShamt;  // 最大移位=numBr = 2
        boost::dynamic_bitset<> folded; // 折叠历史
        std::vector<int> posHighestBitsInGhr; // 最高位在ghr中的位置
        std::vector<int> posHighestBitsInOldFoldedHist; // 最高位在旧折叠历史中的位置

    public:
        FoldedHist(int histLen, int foldedLen, int maxShamt) :
            histLen(histLen), foldedLen(foldedLen), maxShamt(maxShamt)
            {
                folded.resize(foldedLen);
                for (int i = 0; i < maxShamt; i++) {
                    posHighestBitsInGhr.push_back(histLen - 1 - i);
                    posHighestBitsInOldFoldedHist.push_back((histLen - 1 - i) % foldedLen);
                }
            }
    
    public:
        boost::dynamic_bitset<> &get() { return folded; }
        void update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken);
        void recover(FoldedHist &other);
        void check(const boost::dynamic_bitset<> &ghr);
    
};

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_FTB_FOLDED_HIST_HH__
