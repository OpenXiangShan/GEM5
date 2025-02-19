#include "cpu/pred/ftb/folded_hist.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred {

void
FoldedHist::update(const boost::dynamic_bitset<> &ghr, int shamt, bool taken) {
    // 更新折叠历史
    boost::dynamic_bitset<> temp(folded);  // 创建folded的临时副本

    // 情况1：折叠长度大于等于历史长度 (foldedLen >= histLen)
    // 例如：histLen=3, foldedLen=4
    if (foldedLen >= histLen) {
        // 例如：原始folded=1011, shamt=1
        temp <<= shamt;  // 左移1位: 0110
        
        // 将超出histLen的高位置0
        for (int i = histLen; i < foldedLen; i++) {
            temp[i] = 0;  // 0110 -> 0110
        }
        temp[0] = taken;  // 如果taken=1: 0110 -> 0111
    } // 本质就是左移shamt位，设置最低位为taken
    // 情况2：折叠长度小于历史长度 (foldedLen < histLen)，常见
    // 例如：histLen=6, foldedLen=2, shamt=1
    else {
        // 步骤1：扩展temp以容纳移位后的结果
        temp.resize(foldedLen + shamt);  // 2位扩展到3位
        
        // 步骤2：处理由于移位导致的高位变化
        // posHighestBitsInOldFoldedHist和posHighestBitsInGhr是预计算的位置映射
        for (int i = 0; i < shamt; i++) {
            // 将全局历史中的特定位与折叠历史中的对应位异或
            temp[posHighestBitsInOldFoldedHist[i]] ^= ghr[posHighestBitsInGhr[i]];
        }
        
        // 步骤3：执行左移操作
        temp <<= shamt;  
        
        // 步骤4：处理移位后的低位
        for (int i = 0; i < shamt; i++) {
            temp[i] = temp[foldedLen + i];  // 将扩展部分的值复制到低位
        }
        
        // 步骤5：将新的taken值异或到最低位
        temp[0] ^= taken;
        
        // 步骤6：恢复原始大小
        temp.resize(foldedLen);
    }
    
    // 更新folded为新的值
    folded = temp;
}

void
FoldedHist::recover(FoldedHist &other)
{
    assert(foldedLen == other.foldedLen);
    assert(maxShamt == other.maxShamt);
    assert(histLen == other.histLen);
    folded = other.folded;
}

void
FoldedHist::check(const boost::dynamic_bitset<> &ghr)
{
    // Check the folded history now, derive from ghr
    boost::dynamic_bitset<> ideal(ghr);
    boost::dynamic_bitset<> idealFolded;
    ideal.resize(histLen);
    idealFolded.resize(foldedLen);
    for (int i = 0; i < histLen; i++) {
        idealFolded[i % foldedLen] ^= ideal[i];
    }
    assert(idealFolded == folded);
}

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5

/* 折叠举例
   // 假设histLen=6, foldedLen=2的例子
   原始历史：  1 0 1 1 0 1
   分组：      1 0 | 1 1 | 0 1
   对应位置：  [0] [1] | [0] [1] | [0] [1]
   异或结果：  (1^1^0) (0^1^1) = 0 0
   折叠历史：  0 0
*/