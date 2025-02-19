/*
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "base/intmath.hh"
#include "base/stats/info.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/ftb/ftb.hh"
#include "debug/Fetch.hh"
#include "debug/UFTBCount.hh"
namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

DefaultFTB::DefaultFTB(const Params &p)
    : TimedBaseFTBPredictor(p),
    numEntries(p.numEntries),
    tagBits(p.tagBits),
    instShiftAmt(p.instShiftAmt),
    log2NumThreads(floorLog2(p.numThreads)),
    numBr(p.numBr),
    numWays(p.numWays),
    numSets(numEntries / numWays),
    ftbStats(this)
{
    assert(numEntries % numWays == 0);

    if (!isPowerOf2(numEntries)) {
        fatal("FTB entries is not a power of 2!");
    }

    ftb.resize(numSets);
    mruList.resize(numSets);
    for (unsigned i = 0; i < numSets; ++i) {
        for (unsigned j = 0; j < numWays; ++j) {
            ftb[i][0xfffffff-j]; // dummy initialization
        }
        auto &set = ftb[i];
        for (auto it = set.begin(); it != set.end(); it++) {
            it->second.valid = false;
            mruList[i].push_back(it);
        }
        std::make_heap(mruList[i].begin(), mruList[i].end(), older());
    }


    idxMask = numSets - 1;

    tagMask = (1UL << tagBits) - 1;

    tagShiftAmt = instShiftAmt + floorLog2(numSets);
    DPRINTF(FTB, "numEntries %d, numSets %d, numWays %d, tagBits %d, tagShiftAmt %d, idxMask %#lx, tagMask %#lx\n",
        numEntries, numSets, numWays, tagBits, tagShiftAmt, idxMask, tagMask);
}

void
DefaultFTB::tickStart()
{
    // nothing to do
}

void
DefaultFTB::tick() {}

void
DefaultFTB::putPCHistory(Addr startAddr,
                         const boost::dynamic_bitset<> &history,
                         std::vector<FullFTBPrediction> &stagePreds)
{
    TickedFTBEntry find_entry = lookup(startAddr);
    bool hit = find_entry.valid;
    if (hit) {
        // 计算实际基本块大小
        Addr blockSize = 0;
        if (!find_entry.slots.empty()) {  // 如果FTB条目有槽位, 默认最后一个br taken了！但目前没有方向信息，只能暂时这样了，TODO!
            // 使用最后一个分支的PC和大小计算基本块结束位置
            const auto &lastSlot = find_entry.slots.back();
            blockSize = lastSlot.pc + lastSlot.size - startAddr;
        } else {
            // 没有分支时使用fallThruAddr
            blockSize = find_entry.fallThruAddr - startAddr;
        }
        
        // 更新统计
        if (blockSize <= 32) {  // 过滤异常值
            ftbStats.blockSizeDist.sample(blockSize);
        }
        
        // 统计分支数量
        size_t branchNum = find_entry.slots.size();
        ftbStats.branchNumDist.sample(branchNum);
    }
    
    if (hit) { // 2. 统计命中情况
        DPRINTF(FTB, "FTB: lookup hit, dumping hit entry\n");
        DPRINTF(UFTBCount, "%s hit, addr: %#lx index: %#x tag: %#x\n", 
            isL0() ? "UFTB" : "FTB", startAddr, getIndex(startAddr), getTag(startAddr));
        ftbStats.predHit++;
        printTickedFTBEntry(find_entry);
    } else {
        ftbStats.predMiss++;

        DPRINTF(FTB, "FTB: lookup miss\n");
    }
    assert(getDelay() < stagePreds.size());
    // assign prediction for s2 and later stages // 3. 为流水线各级填充预测结果
    for (int s = getDelay(); s < stagePreds.size(); ++s) {  // s = 0,1,2(uFTB)  or  1,2(FTB)
        if (!isL0() && !hit && stagePreds[s].valid) {  // 不在L0且FTB未命中, 但是有预测结果？
            DPRINTF(FTB, "FTB: uftb hit and ftb miss, use uftb result");// 3.1 处理L0 FTB未命中但uFTB命中的情况
            incNonL0Stat(ftbStats.predUseL0OnL1Miss);
            break;
        }
        DPRINTF(FTB, "FTB: assigning prediction for stage %d\n", s);
        stagePreds[s].valid = hit; // 3.2 基本预测信息， 更新预测结果
        stagePreds[s].ftbEntry = find_entry;  // 更新FTB条目传到预测结果
        DPRINTF(FTB, "FTB: numBranches %d\n", numBr);

        if (isL0() && s == 0) { // 3.3 L0 FTB的条件分支预测, uftb应该只在0级更新吧？
            // use saturating counter of L0 uFTB 用两位饱和计数器
            for (int i = 0; i < numBr; ++i) {   // 遍历每个槽
                if (find_entry.slots.size() > i) {  // 如果包含这个分支i, 也就是有槽位
                    stagePreds[s].condTakens[i] = find_entry.slots[i].ctr >= 0 && hit;  // ctr >= 0 且命中,记录为taken
                    DPRINTF(UFTBCount, "UFTBCount: i = %d, numBr = %d, cond taken %d, ctr: %d, hit: %d\n", 
                        i, numBr, stagePreds[s].condTakens[i], find_entry.slots[i].ctr, hit);
                } else {
                    stagePreds[s].condTakens[i] = false; // 没有槽位， 预测为不跳转
                }
            }
        }
        // assign ftb prediction for indirect targets // 3.4 间接跳转目标预测
        if (!find_entry.slots.empty()) {  // 如果包含槽位
            auto tail_slot = find_entry.slots.back();  // 获取最后一个槽位
            if (tail_slot.uncondValid()) {  // 如果最后一个槽位是无条件跳转有效
                stagePreds[s].indirectTarget = tail_slot.target;  // 设置间接跳转目标
                if (tail_slot.isReturn) {  // 如果是返回指令，设置返回地址
                    stagePreds[s].returnTarget = tail_slot.target;
                }
            }
        }
        stagePreds[s].predTick = curTick();  // 更新预测时间
    } // 4. 更新元数据
    if (getDelay() >= 1) {  // 是FTB
        meta.l0_hit = stagePreds[getDelay() - 1].valid;  // 更新L0命中情况, 0级命中了
    }
    meta.hit = hit;  // 更新命中情况
    meta.entry = FTBEntry(find_entry);  // 更新FTB条目
}

std::shared_ptr<void>
DefaultFTB::getPredictionMeta()
{
    std::shared_ptr<void> meta_void_ptr = std::make_shared<FTBMeta>(meta);
    return meta_void_ptr;
}

void
DefaultFTB::specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) {
    // if (!isL0()) return;  // 只对uFTB做推测更新
    
    // // 获取当前预测的FTB条目
    // Addr startPC = pred.bbStart;
    // Addr ftb_idx = getIndex(startPC);
    // Addr ftb_tag = getTag(startPC);
    
    // auto it = ftb[ftb_idx].find(ftb_tag);
    // if (it == ftb[ftb_idx].end()) return;  // 未命中则不更新
    
    // auto &ftb_entry = it->second;
    
    // // 遍历所有条件分支槽位
    // for (int i = 0; i < numBr && i < ftb_entry.slots.size(); i++) {
    //     auto &slot = ftb_entry.slots[i];
    //     if (!slot.isCond || slot.alwaysTaken) continue;  // 只更新条件分支且非always taken
        
    //     // 获取预测结果
    //     bool pred_taken = pred.condTakens[i];
        
    //     // 推测性更新计数器
    //     int new_ctr = slot.ctr;
    //     DPRINTF(UFTBCount, "spec update: pc =%#lx old=%d\n", startPC, slot.ctr);
    //     updateCtr(new_ctr, pred_taken);
        
    //     // 更新FTB条目中的计数器
    //     slot.ctr = new_ctr;
        
    //     DPRINTF(UFTBCount, "Spec update ctr for branch %d: old=%d new=%d pred_taken=%d\n",
    //             i, slot.ctr, new_ctr, pred_taken);
    // }
}

void
DefaultFTB::reset()
{
    for (unsigned i = 0; i < numSets; ++i) {
        for (unsigned j = 0; j < numWays; ++j) {
            ftb[i][j].valid = false;
        }
    }
}

inline
Addr
DefaultFTB::getIndex(Addr instPC)
{
    // Need to shift PC over by the word offset.
    return (instPC >> instShiftAmt) & idxMask;
}

inline
Addr
DefaultFTB::getTag(Addr instPC)
{
    return (instPC >> tagShiftAmt) & tagMask;
}

bool
DefaultFTB::valid(Addr instPC)
{
    Addr ftb_idx = getIndex(instPC);

    Addr inst_tag = getTag(instPC);

    assert(ftb_idx < numEntries);

    for (int w = 0; w < numWays; w++) {
        if (ftb[ftb_idx][w].valid
            && inst_tag == ftb[ftb_idx][w].tag) {
            return true;
        }
    }
    return false;
}

// @todo Create some sort of return struct that has both whether or not the
// address is valid, and also the address.  For now will just use addr = 0 to
// represent invalid entry.
DefaultFTB::TickedFTBEntry
DefaultFTB::lookup(Addr inst_pc)
{
    if (inst_pc & 0x1) {  // 最低位是1, 忽略
        return TickedFTBEntry(); // ignore false hit when lowest bit is 1
    }
    Addr ftb_idx = getIndex(inst_pc);

    Addr ftb_tag = getTag(inst_pc);
    DPRINTF(FTB, "FTB: Looking up FTB entry index %#lx tag %#lx\n", ftb_idx, ftb_tag);

    assert(ftb_idx < numSets);
    // ignore false hit when lowest bit is 1
    const auto &it = ftb[ftb_idx].find(ftb_tag);  // 根据index查找tag
    if (it != ftb[ftb_idx].end()) {  // 找到
        if (it->second.valid) {  // 有效
            it->second.tick = curTick();
            std::make_heap(mruList[ftb_idx].begin(), mruList[ftb_idx].end(), older());  // 更新MRU列表
            return it->second;
        }
    }
    return TickedFTBEntry();  // 未找到
}

void
DefaultFTB::getAndSetNewFTBEntry(FetchStream &stream)
{
    DPRINTF(FTB, "generating new ftb entry\n");
    // generate ftb entry
    Addr startPC = stream.getRealStartPC();
    Addr inst_tag = getTag(startPC);


    bool pred_hit = stream.isHit;  // ftb命中

    bool stream_taken = stream.exeTaken;  // 执行阶段分支确实跳转
    FTBEntry entry_to_write;  // 要写入的FTB条目
    bool is_old_entry = pred_hit;
    if (pred_hit || stream_taken) { // 如果预测命中或执行跳转, 调用者update()也是这样
        BranchInfo branch_info = stream.exeBranchInfo;  // 执行阶段那条分支信息
        bool is_uncond = branch_info.isUncond();    // 当前分支是否是无条件分支
        // if pred not hit, establish a new entry
        if (!pred_hit) { // 如果预测未命中， 执行跳转了，建立新的FTB条目
            DPRINTF(FTB, "pred miss, creating new FTB entry\n");
            FTBEntry new_entry;
            new_entry.valid = true;
            new_entry.tag = inst_tag;
            std::vector<FTBSlot> &slots = new_entry.slots;
            FTBSlot new_slot = FTBSlot(branch_info);  // 创建新的槽位，taken的br作为第一个分支
            slots.push_back(new_slot);
            // uncond branch should set fallThruAddr to end of that inst 无条件分支设置fallThruAddr为指令结束地址
            if (is_uncond) {
                new_entry.fallThruAddr = branch_info.getEnd();  // 无条件分支直接设置fallThruAddr为指令结束地址=pc+4
                incNonL0Stat(ftbStats.newEntryWithUncond);  // 无条件分支
            } else {
                new_entry.fallThruAddr = startPC + 32;  // 条件分支设置fallThruAddr为当前PC+32？
                incNonL0Stat(ftbStats.newEntryWithCond);
            }
            entry_to_write = new_entry;
            incNonL0Stat(ftbStats.newEntry);
        } else { // 如果预测命中，需要时候更新FTB条目
            DPRINTF(FTB, "pred hit, updating FTB entry if necessary\n");
            DPRINTF(FTB, "printing old entry:\n");
            FTBEntry old_entry = stream.predFTBEntry;
            printFTBEntry(old_entry);
            // assert(old_entry.tag == inst_tag && old_entry.valid);
            std::vector<FTBSlot> &slots = old_entry.slots;
            bool new_branch = !branchIsInEntry(old_entry, branch_info.pc);  // 如果分支不在FTB现有分支中，需要插入进去
            if (new_branch && stream_taken) {  // 如果分支不在FTB条目中且执行跳转, 插入新的槽位并移除多余的
                is_old_entry = false;
                DPRINTF(FTB, "new taken branch detected, inserting into FTB entry\n");
                // keep pc ascending order 插入新的槽位并保持pc升序
                auto it = slots.begin();
                while (it != slots.end()) {
                    if (*it > branch_info) {
                        break;
                    }
                    ++it;
                }
                slots.insert(it, FTBSlot(branch_info));  // 插入新的槽位到正确pc位置
                // remove the last slot if there are more than numBr slots
                if (slots.size() > numBr) { // 如果槽位数超过numBr, 移除最后一个槽位
                    DPRINTF(FTB, "removing last slot because there are more than %d slots", numBr);
                    Addr last_slot_pc = slots.rbegin()->pc;     // 获取最后一个槽位的pc
                    slots.pop_back();  // 移除最后一个槽位
                    old_entry.fallThruAddr = last_slot_pc;  // 设置fallThruAddr为最后一个槽位的pc
                }
                // ensure uncond slot is the tail slot
                // Note: if an unconditional jump has a target equal to fallThruPC,
                //       predicting it to be not taken will not be considered a mispredict
                //       thus an ftq entry would possibly has two taken branches inside,
                //       among which the first being an unconditional jump to its fallThruPC
                // 确保无条件分支是最后一个槽位
                // Note: 如果一个无条件分支的目标等于fallThruPC，预测它不跳转不会被认为是错误预测，刚好跳到fallThruPC,概率小
                //       因此，一个FTQ条目可能有两个跳转分支，其中一个是无条件跳转到它的fallThruPC，概率小
                // 是uncond分支且在FTB条目中，新插入uncond就在entry中了，现在保证j在最后
                if (branch_info.isUncond() && branchIsInEntry(old_entry, branch_info.pc)) {
                    // check if there is other branches behind an indirect jump
                    // remove slots behind an unconditional jump
                    // 检查是否存在间接跳转后的其他分支，移除间接跳转后的分支
                    FTBSlot back = slots.back();
                    while (slots.back() > branch_info) {
                        DPRINTF(FTB, "erasing slot behind uncond slot:\n"); // 移除间接跳转后的任何分支
                        DPRINTF(FTB, "    pc:%#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d, always_taken:%d\n",
                            back.pc, back.size, back.target, back.isCond, back.isIndirect, back.isCall, back.isReturn, back.alwaysTaken);
                        slots.pop_back();
                        back = slots.back();
                    }
                    assert(back == branch_info);
                    DPRINTF(FTB, "setting fallThruAddr to the next inst of uncond: %#lx\n", old_entry.fallThruAddr);
                    old_entry.fallThruAddr = branch_info.pc + branch_info.size; // 设置fallThruAddr为uncond的下一个指令地址，有何用？反正都要taken的，就是减少一点方向预测错误开销
                }
                if (branch_info.isCond) {
                    incNonL0Stat(ftbStats.oldEntryWithNewCond);  // 有新条件分支
                } else {
                    incNonL0Stat(ftbStats.oldEntryWithNewUncond);  // 有新无条件分支
                }
            }
            if (!new_branch && branch_info.isIndirect && stream_taken) { // 如果分支不是新分支且是间接跳转且执行跳转
                auto &tailSlot = slots.back();  // 获取最后一个槽位
                assert(tailSlot.isIndirect);  // 确保是间接跳转
                if (tailSlot.target != branch_info.target) {  // 如果间接跳转目标与执行分支目标不同， j0,j1, 选j0
                    tailSlot.target = branch_info.target;  // 更新间接跳转目标
                    is_old_entry = false;  // 设置为旧条目
                    incNonL0Stat(ftbStats.oldEntryIndirectTargetModified);  // 更新统计, 间接跳转目标修改
                }
            }
            // modify always taken logic 修改always taken逻辑(原本一直taken, 现在不taken 了)
            auto it = slots.begin();
            while (it != slots.end()) {
                // set branches before current branch to alwaysTaken: false 设置之前分支为always taken: false
                if (*it < branch_info) { // 如果当前分支在分支信息之前
                    if (it->alwaysTaken) { // 如果当前分支是always taken
                        it->alwaysTaken = false; // 设置为不taken
                        is_old_entry = false; // 设置为旧条目
                    }
                }
                // current always taken branch not taken: alwaysTaken false 当前always taken分支不taken: alwaysTaken false
                else if (*it == branch_info && it->alwaysTaken && !stream_taken) {
                    is_old_entry = false;
                    it->alwaysTaken = false;
                }
                it++;
            }
            entry_to_write = old_entry;
            incNonL0Stat(ftbStats.oldEntry);
        }
        DPRINTF(FTB, "printing new entry:\n");
        printFTBEntry(entry_to_write);
        checkFTBEntry(entry_to_write);
    }
    stream.updateFTBEntry = entry_to_write;  // 更新要写入的FTB条目
    stream.updateIsOldEntry = is_old_entry;
}

void
DefaultFTB::update(const FetchStream &stream)
{ // 用commit stream更新预测器内容, 准确的！
    auto meta = std::static_pointer_cast<FTBMeta>(stream.predMetas[getComponentIdx()]); // 获取预测时的meta
    if (meta->hit) {
        ftbStats.updateHit++;
    } else {
        ftbStats.updateMiss++;
    }
    if (!isL0()) { // L1 FTB特殊处理：如果L0命中但L1未命中，跳过更新
        bool l0_hit_l1_miss = meta->l0_hit && !meta->hit; // L0命中但L1未命中
        if (l0_hit_l1_miss) {
            DPRINTF(FTB, "FTB: skipping entry write because of l0 hit\n");
            incNonL0Stat(ftbStats.updateUseL0OnL1Miss);
            return;
        }
    }
    Addr startPC = stream.getRealStartPC();     // 获取stream真实PC来更新对应的FTB条目
    Addr ftb_idx = getIndex(startPC);  // 获取FTB索引
    Addr ftb_tag = getTag(startPC);  // 获取FTB标签


    DPRINTF(FTB, "FTB: Updating FTB entry index %#lx tag %#lx\n", ftb_idx, ftb_tag);

    auto it = ftb[ftb_idx].find(ftb_tag);
    // if the tag is not found and the table is full
    bool not_found = it == ftb[ftb_idx].end();

    if (not_found) { // 如果未找到且表满，执行LRU替换
        std::pop_heap(mruList[ftb_idx].begin(), mruList[ftb_idx].end(), older()); // 从MRU列表中移除最后一个元素
        const auto& old_entry = mruList[ftb_idx].back();
        DPRINTF(FTB, "FTB: Replacing entry with tag %#lx in set %#lx\n", old_entry->first, ftb_idx);
        ftb[ftb_idx].erase(old_entry->first); // 从FTB中移除旧条目

        ftbStats.replacements++; // 替换次数
        ftbStats.fullSetEvents++; // 表满次数
    }

    auto updatedEntry = stream.updateFTBEntry;  // 要写入的FTB条目
    bool updatedIsOldEntry = stream.updateIsOldEntry; // 是否是旧条目
    auto entryInFtbNow = ftb[ftb_idx][ftb_tag];  // 当前FTB条目， 根据idx和tag得到，要写入的位置
    // if this entry is old entry, use entry now in ftb to avoid overwriting entry with more branche info
    // 如果是旧条目且当前FTB中存在，使用现有条目避免覆盖更多分支信息
    auto entry_to_write = (updatedIsOldEntry && !not_found) ? FTBEntry(entryInFtbNow) : updatedEntry;
    // train L0 FTB ctrs  L0 FTB ctr 更新
    if (isL0()) {   // UFTB更新ctrs
        std::vector<bool> need_to_update;
        need_to_update.resize(numBr, false);
        auto &ftb_entry = entry_to_write;
        // get number of conditional branches to update
        int cond_num = 0;  // 获取累计的条件分支数量，小于numBr
        if (stream.exeTaken) { // 如果执行分支命中
            // 获取执行分支前的条件分支数
            cond_num = ftb_entry.getNumCondInEntryBefore(stream.exeBranchInfo.pc);
            // for case of ftb entry is not full 如果FTB条目未满
            if (cond_num < numBr) {
                cond_num += !stream.exeBranchInfo.isUncond() ? 1 : 0; // 如果执行分支不是无条件分支，则条件分支数加1
            }
            // if ftb entry is full, and this branch is conditional,
            // we cannot update the last branch, as it will be removed
            // from current ftb entry
            // 如果FTB条目已满，且当前分支是有条件分支，则无法更新最后一个分支，因为它将被从当前FTB条目中移除
        } else {
            // corresponding to RTL, but in fact we should consider
            // whether the branches are flushed
            // TODO: fix it and check whether it can bring performance improvement
            cond_num = ftb_entry.getTotalNumConds();
        }
        assert(cond_num <= numBr);

        // assert(cond_num <= ftb_entry.slots.size());
        cond_num = std::min(cond_num, (int)ftb_entry.slots.size());
        for (int i = 0; i < cond_num; i++) {
            auto &slot = ftb_entry.slots[i];
            // only update branches with both taken/not taken behaviors observed 只更新有taken/not taken行为的分支
            need_to_update[i] = !slot.alwaysTaken; // 只更新非alwaysTaken的分支
        }
        for (int b = 0; b < numBr; b++) {  // 更新分支预测计数器
            if (!need_to_update[b]) { // 如果不需要更新，跳过
                continue;
            }
            // 确定当前条件分支是否实际命中 = exe分支跳转了 且是当前分支
            bool this_cond_actually_taken = stream.exeTaken && stream.exeBranchInfo == ftb_entry.slots[b];
            int ctr_to_be_updated;
            // read newest ctr if hit // 获取最新的计数器值
            if (!not_found && it->second.slots.size() > b) { // 找到且槽位数大于b
                ctr_to_be_updated = entryInFtbNow.slots[b].ctr; // 获取当前槽位的计数器
            } else {
                ctr_to_be_updated = updatedEntry.slots[b].ctr;
            }
            DPRINTF(UFTBCount, "UFTBCount: updating ctr %d, taken = %d for branch %d\n", ctr_to_be_updated, this_cond_actually_taken, b);
            updateCtr(ctr_to_be_updated, this_cond_actually_taken); // 更新计数器
            DPRINTF(UFTBCount, "UFTBCount: updated ctr %d\n", ctr_to_be_updated);
            entry_to_write.slots[b].ctr = ctr_to_be_updated; // 更新槽位的计数器
        }
    }

    ftb[ftb_idx][ftb_tag] = TickedFTBEntry(entry_to_write, curTick());  // 更新FTB条目
    ftb[ftb_idx][ftb_tag].tag = ftb_tag; // in case different ftb has different tags 可能不同ftb项有不同tag


    if (not_found) {
        auto it = ftb[ftb_idx].find(ftb_tag);
        assert(it != ftb[ftb_idx].end());
        mruList[ftb_idx].back() = it; // 将新条目添加到MRU列表的末尾
        std::push_heap(mruList[ftb_idx].begin(), mruList[ftb_idx].end(), older()); // 将新条目添加到MRU列表中
    } else {
        std::make_heap(mruList[ftb_idx].begin(), mruList[ftb_idx].end(), older());
    }
    assert(ftb_idx < numSets); // 确保idx在numSets范围内
    assert(ftb[ftb_idx].size() <= numWays); // 确保FTB条目数在numWays范围内
    assert(mruList[ftb_idx].size() <= numWays); // 确保MRU列表条目数在numWays范围内

    // ftbStats.setUsage[ftb_idx]++;
    // ftbStats.wayUsage[ftb[ftb_idx].size()]++;

    // ftb[ftb_idx].valid = true;
    // set(ftb[ftb_idx].target, target);
    // ftb[ftb_idx].tag = getTag(inst_pc);
}

void
DefaultFTB::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
{ // commit阶段统计分支数据
    auto meta = std::static_pointer_cast<FTBMeta>(stream.predMetas[getComponentIdx()]); // 获取预测元数据
    auto &entry = meta->entry; // 获取FTB条目
    auto pc = inst->getPC(); // 获取PC
    auto npc = inst->getNPC(); // 获取NPC
    // auto &static_inst = inst->staticInst();
    bool this_branch_hit = meta->hit && branchIsInEntry(entry, pc); // 命中，分支是否在FTB条目中
    // bool this_branch_miss = !this_branch_hit;
    bool cond_not_taken = inst->isCondCtrl() && !inst->branching(); // 条件分支，且不跳转
    bool this_branch_taken = !cond_not_taken; // 条件分支，且跳转
    Addr this_branch_target = npc;
    const auto &slot = entry.getSlot(pc);
    if (this_branch_hit) { // 命中
        ftbStats.allBranchHits++;
        if (this_branch_taken) { // 命中且taken
            ftbStats.allBranchHitTakens++;
        } else {
            ftbStats.allBranchHitNotTakens++;
        }
        if (inst->isCondCtrl()) {
            ftbStats.condHits++;
            if (this_branch_taken) {
                ftbStats.condHitTakens++;
            } else {
                ftbStats.condHitNotTakens++;
            }
            if (isL0()) {
                bool pred_taken = slot.ctr >= 0;
                if (pred_taken == this_branch_taken) { // 预测正确
                    ftbStats.condPredCorrect++;
                } else {
                    ftbStats.condPredWrong++;
                }
            }
        }
        if (inst->isUncondCtrl()) {
            ftbStats.uncondHits++;
        }
        // ignore non-speculative branches (e.g. syscall)
        if (!inst->isNonSpeculative()) {
            if (inst->isIndirectCtrl()) {
                ftbStats.indirectHits++;
                Addr pred_target = slot.target;
                if (pred_target == this_branch_target) {
                    ftbStats.indirectPredCorrect++;
                } else {
                    ftbStats.indirectPredWrong++;
                }
            }
            if (inst->isCall()) {
                ftbStats.callHits++;
            }
            if (inst->isReturn()) {
                ftbStats.returnHits++;
            }
        }
    } else { // 未命中
        ftbStats.allBranchMisses++;
        if (this_branch_taken) {
            ftbStats.allBranchMissTakens++;
        } else {
            ftbStats.allBranchMissNotTakens++;
        }
        if (inst->isCondCtrl()) {
            ftbStats.condMisses++;
            if (this_branch_taken) { // 条件分支，且taken
                ftbStats.condMissTakens++;
                if (isL0()) {
                    // only L0 FTB has saturating counters to predict conditional branches
                    // taken branches that is missed in ftb must have been mispredicted
                    // L0 FTB有饱和计数器来预测条件分支
                    // 在FTB中未命中的taken分支必须被错误预测
                    ftbStats.condPredWrong++;
                }
            } else { // 条件分支，且not taken
                ftbStats.condMissNotTakens++;
                if (isL0()) {
                    // only L0 FTB has saturating counters to predict conditional branches
                    // taken branches that is missed in ftb must have been mispredicted
                    ftbStats.condPredCorrect++;     // 预测NT， 实际NT 不存，也算预测正确
                }
            }
        }
        if (inst->isUncondCtrl()) {
            ftbStats.uncondMisses++;
        }
        // ignore non-speculative branches (e.g. syscall)
        if (!inst->isNonSpeculative()) {
            if (inst->isIndirectCtrl()) {
                ftbStats.indirectMisses++;
                ftbStats.indirectPredWrong++;
            }
            if (inst->isCall()) {
                ftbStats.callMisses++;
            }
            if (inst->isReturn()) {
                ftbStats.returnMisses++;
            }
        }
    }
}

DefaultFTB::FTBStats::FTBStats(statistics::Group* parent) :
    statistics::Group(parent),
    ADD_STAT(newEntry, statistics::units::Count::get(), "number of new ftb entries generated"),
    ADD_STAT(newEntryWithCond, statistics::units::Count::get(), "number of new ftb entries generated with conditional branch"),
    ADD_STAT(newEntryWithUncond, statistics::units::Count::get(), "number of new ftb entries generated with unconditional branch"),
    ADD_STAT(oldEntry, statistics::units::Count::get(), "number of old ftb entries updated"),
    ADD_STAT(oldEntryIndirectTargetModified, statistics::units::Count::get(), "number of old ftb entries with indirect target modified"),
    ADD_STAT(oldEntryWithNewCond, statistics::units::Count::get(), "number of old ftb entries with new conditional branches"),
    ADD_STAT(oldEntryWithNewUncond, statistics::units::Count::get(), "number of old ftb entries with new unconditional branches"),
    ADD_STAT(predMiss, statistics::units::Count::get(), "misses encountered on prediction"),
    ADD_STAT(predHit, statistics::units::Count::get(), "hits encountered on prediction"),
    ADD_STAT(updateMiss, statistics::units::Count::get(), "misses encountered on update"),
    ADD_STAT(updateHit, statistics::units::Count::get(), "hits encountered on update"),
    ADD_STAT(eraseSlotBehindUncond, statistics::units::Count::get(), "erase slots behind unconditional slot"),
    ADD_STAT(predUseL0OnL1Miss, statistics::units::Count::get(), "use l0 result on l1 miss when pred"),
    ADD_STAT(updateUseL0OnL1Miss, statistics::units::Count::get(), "use l0 result on l1 miss when update"),

    ADD_STAT(allBranchHits, statistics::units::Count::get(), "all types of branches committed that was predicted hit"),
    ADD_STAT(allBranchHitTakens, statistics::units::Count::get(), "all types of taken branches committed was that predicted hit"),
    ADD_STAT(allBranchHitNotTakens, statistics::units::Count::get(), "all types of not taken branches committed was that predicted hit"),
    ADD_STAT(allBranchMisses, statistics::units::Count::get(), "all types of branches committed that was predicted miss"),
    ADD_STAT(allBranchMissTakens, statistics::units::Count::get(), "all types of taken branches committed was that predicted miss"),
    ADD_STAT(allBranchMissNotTakens, statistics::units::Count::get(), "all types of not taken branches committed was that predicted miss"),
    ADD_STAT(condHits, statistics::units::Count::get(), "conditional branches committed that was predicted hit"),
    ADD_STAT(condHitTakens, statistics::units::Count::get(), "taken conditional branches committed was that predicted hit"),
    ADD_STAT(condHitNotTakens, statistics::units::Count::get(), "not taken conditional branches committed was that predicted hit"),
    ADD_STAT(condMisses, statistics::units::Count::get(), "conditional branches committed that was predicted miss"),
    ADD_STAT(condMissTakens, statistics::units::Count::get(), "taken conditional branches committed was that predicted miss"),
    ADD_STAT(condMissNotTakens, statistics::units::Count::get(), "not taken conditional branches committed was that predicted miss"),
    ADD_STAT(condPredCorrect, statistics::units::Count::get(), "conditional branches committed was that correctly predicted by ftb"),
    ADD_STAT(condPredWrong, statistics::units::Count::get(), "conditional branches committed was that mispredicted by ftb"),
    ADD_STAT(uncondHits, statistics::units::Count::get(), "unconditional branches committed that was predicted hit"),
    ADD_STAT(uncondMisses, statistics::units::Count::get(), "unconditional branches committed that was predicted miss"),
    ADD_STAT(indirectHits, statistics::units::Count::get(), "indirect branches committed that was predicted hit"),
    ADD_STAT(indirectMisses, statistics::units::Count::get(), "indirect branches committed that was predicted miss"),
    ADD_STAT(indirectPredCorrect, statistics::units::Count::get(), "indirect branches committed whose target was correctly predicted by ftb"),
    ADD_STAT(indirectPredWrong, statistics::units::Count::get(), "indirect branches committed whose target was mispredicted by ftb"),
    ADD_STAT(callHits, statistics::units::Count::get(), "calls committed that was predicted hit"),
    ADD_STAT(callMisses, statistics::units::Count::get(), "calls committed that was predicted miss"),
    ADD_STAT(returnHits, statistics::units::Count::get(), "returns committed that was predicted hit"),
    ADD_STAT(returnMisses, statistics::units::Count::get(), "returns committed that was predicted miss"),
    // ADD_STAT(setUsage, statistics::units::Count::get(), "Usage count per set"),
    // ADD_STAT(wayUsage, statistics::units::Count::get(), "Way usage distribution per set"),
    ADD_STAT(replacements, statistics::units::Count::get(), "Number of entry replacements"),
    ADD_STAT(fullSetEvents, statistics::units::Count::get(), "Number of times a set was full"),
    ADD_STAT(blockSizeDist, 
        "Distribution of basic block sizes in bytes"),
    ADD_STAT(branchNumDist,
        "Distribution of number of branches per FTB entry")
{
    auto ftb = dynamic_cast<branch_prediction::ftb_pred::DefaultFTB*>(parent);
    
    // setUsage.init(ftb->numSets+1).flags(statistics::total);
    // wayUsage.init(ftb->numWays+1).flags(statistics::total);
    // do not need counter below in L0 ftb
    if (ftb->isL0()) {
        predUseL0OnL1Miss.prereq(predUseL0OnL1Miss);
        updateUseL0OnL1Miss.prereq(updateUseL0OnL1Miss);
        newEntry.prereq(newEntry);
        newEntryWithCond.prereq(newEntryWithCond);
        newEntryWithUncond.prereq(newEntryWithUncond);
        oldEntry.prereq(oldEntry);
        oldEntryIndirectTargetModified.prereq(oldEntryIndirectTargetModified);
        oldEntryWithNewCond.prereq(oldEntryWithNewCond);
        oldEntryWithNewUncond.prereq(oldEntryWithNewUncond);
        eraseSlotBehindUncond.prereq(eraseSlotBehindUncond);
    }

    // 基本块大小分布: 0-32字节,步长4字节
    blockSizeDist.init(0, 32, 4)
        .flags(statistics::pdf | statistics::cdf);
        
    // 分支数量分布: 0到numBr个分支
    branchNumDist.init(0, 4, 1)
        .flags(statistics::pdf | statistics::cdf);
}

} // namespace ftb_pred
} // namespace branch_prediction
} // namespace gem5
