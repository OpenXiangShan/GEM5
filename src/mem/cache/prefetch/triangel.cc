
/*
 * Copyright (c) 2023
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2005 The Regents of The University of Michigan
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

/**
 * @file
 * Stride Prefetcher template instantiations.
 */
#include "mem/cache/prefetch/triangel.hh"

#include <cmath>
#include <utility>
#include <vector>

#include "debug/TriangelDebug.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "mem/request.hh"
#include "params/TriangelPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

int Triangel::target_size = 0;
int Triangel::current_size = 0;
int64_t Triangel::global_timestamp = 0;
AssociativeSet<Triangel::MarkovMapping> *Triangel::markovTablePtr = NULL;
std::vector<uint32_t> Triangel::setPrefetch(17, 0);
Triangel::SizeDuel *Triangel::sizeDuelPtr = nullptr;
bloom *Triangel::blptr = nullptr;

Triangel::Triangel(const TriangelPrefetcherParams &p)
    : Queued(p),
      degree(p.degree),
      cachetags(p.cachetags),
      cacheDelay(p.cache_delay),
      should_lookahead(p.should_lookahead),
      should_rearrange(p.should_rearrange),
      use_scs(p.use_scs),
      use_bloom(p.use_bloom),
      use_reuse(p.use_reuse),
      use_pattern(p.use_pattern),
      use_pattern2(p.use_pattern2),
      use_mrb(p.use_mrb),
      perfbias(p.perfbias),
      smallduel(p.smallduel),
      timed_scs(p.timed_scs),
      useSampleConfidence(p.useSampleConfidence),
      max_size(p.address_map_actual_entries),
      size_increment(p.address_map_actual_entries / p.address_map_max_ways),
      second_chance_timestamp(0),
      maxWays(p.address_map_max_ways),
      bl(),
      bloomset(-1),
      way_idx(p.address_map_actual_entries / (p.address_map_max_ways * p.address_map_actual_cache_assoc), 0),
      globalReuseConfidence(7, 64),
      globalPatternConfidence(7, 64),
      globalHighPatternConfidence(7, 64),
      trainingUnit(p.training_unit_assoc, p.training_unit_entries, p.training_unit_indexing_policy,
                   p.training_unit_replacement_policy),
      lookupAssoc(p.lookup_assoc),
      lookupOffset(p.lookup_offset),
      // setPrefetch(cachetags->getWayAllocationMax()+1,0),
      useHawkeye(p.use_hawkeye),
      historySampler(p.sample_assoc, p.sample_entries, p.sample_indexing_policy, p.sample_replacement_policy),
      secondChanceUnit(p.secondchance_assoc, p.secondchance_entries, p.secondchance_indexing_policy,
                       p.secondchance_replacement_policy),
      markovTable(p.address_map_rounded_cache_assoc, p.address_map_rounded_entries,
                  p.address_map_cache_indexing_policy, p.address_map_cache_replacement_policy, MarkovMapping()),
      metadataReuseBuffer(p.metadata_reuse_assoc, p.metadata_reuse_entries, p.metadata_reuse_indexing_policy,
                          p.metadata_reuse_replacement_policy, MarkovMapping()),
      lastAccessFromPFCache(false),
      triangelStats(this)
{
    markovTablePtr = &markovTable;

    setPrefetch.resize(cachetags->getWayAllocationMax() + 1, 0);

    // assert 每个 cache way 上能容纳的Markov表条目数一样
    assert(p.address_map_rounded_entries / p.address_map_rounded_cache_assoc ==
           p.address_map_actual_entries / p.address_map_actual_cache_assoc);
    markovTable.setWayAllocationMax(p.address_map_actual_cache_assoc);
    assert(cachetags->getWayAllocationMax() > maxWays);
    int bloom_size = p.address_map_actual_entries / 128 < 1024 ? 1024 : p.address_map_actual_entries / 128;
    assert(bloom_init2(&bl, bloom_size, 0.01) == 0);
    blptr = &bl;
    for (int x = 0; x < 64; x++) {
        hawksets[x].setMask = p.address_map_actual_entries / hawksets[x].maxElems;
        hawksets[x].reset();
    }
    sizeDuelPtr = sizeDuels;
    for (int x = 0; x < 64; x++) {
        sizeDuelPtr[x].reset(size_increment / p.address_map_actual_cache_assoc - 1, p.address_map_actual_cache_assoc,
                             cachetags->getWayAllocationMax());
    }

    for (int x = 0; x < 1024; x++) {
        lookupTable[x] = 0;
        lookupTick[x] = 0;
    }
    current_size = 0;
    target_size = 0;
}


bool
Triangel::randomChance(int reuseConf, int replaceRate)
{
    replaceRate -= 8;

    uint64_t baseChance = 1000000000l * historySampler.numEntries / markovTable.numEntries;
    baseChance = replaceRate > 0 ? (baseChance << replaceRate) : (baseChance >> (-replaceRate));
    baseChance = reuseConf < 3 ? baseChance / 16 : baseChance;
    uint64_t chance = random_mt.random<uint64_t>(0, 1000000000ul);

    return baseChance >= chance;
}

void
Triangel::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{

    Addr addr = blockIndex(pfi.getAddr());
    second_chance_timestamp++;

    // This prefetcher requires a PC
    if (!pfi.hasPC() || pfi.isWrite()) {
        if (!use_bloom) {
            for (int x = 0; x < (smallduel ? 32 : 64); x++) {
                int res = sizeDuelPtr[x].checkAndInsert(addr, false);
                if (res == 0)
                    continue;
                int cache_hit = res % 128;
                int cache_set = cache_hit - 1;
                assert(!cache_hit || (cache_set < setPrefetch.size() - 1 && cache_set >= 0));
                if (cache_hit)
                    for (int y = setPrefetch.size() - 2 - cache_set; y >= 0; y--)
                        setPrefetch[y]++;
                // cache partition hit at this size or bigger. So hit in way 14 = y=17-2-14=1 and 0: would hit with 0
                // ways reserved or 1, not 2.
            }
        }
        return;
    }

    bool is_secure = pfi.isSecure();
    // Shifted by 2 to help Arm indexing. Bit fake; really should xor in these bits with upper bits.
    Addr pc = pfi.getPC() >> 2;



    bool correlated_addr_found = false;
    Addr index = 0;
    Addr target = 0;

    const int upperHistory = globalPatternConfidence > 64 ? 7 : 8;
    const int highUpperHistory = globalHighPatternConfidence > 64 ? 7 : 8;
    const int superHistory = 14;

    const int upperReuse = globalReuseConfidence > 64 ? 7 : 8;

    // const int globalThreshold = 9;

    bool should_pf = false;      // 是否应该进行预取的标志
    bool should_hawk = false;    // 是否应该使用Hawkeye算法的标志
    bool should_sample = false;  // 是否应该进行采样的标志

    // 在训练单元中查找与当前PC关联的条目
    // 训练单元存储每个PC的访问历史信息，用于学习访问模式
    TrainingUnitEntry *originalTrainEntry = trainingUnit.findEntry(pc, is_secure);
    if (originalTrainEntry != nullptr) {  // 如果在训练表中找到了该PC的条目
        // 更新该条目的访问时间戳，标记为最近访问
        trainingUnit.accessEntry(originalTrainEntry);
        correlated_addr_found = true;             // 标记找到了关联地址
        index = originalTrainEntry->lastAddress;  // 获取该PC上次访问的地址作为索引

        // 避免对相同地址序列进行重复训练
        // 如果当前访问地址与上次访问地址相同，直接返回
        if (addr == originalTrainEntry->lastAddress)
            return;

        // 根据高模式置信度决定是否启用两距离预取
        // 如果高模式置信度很高(>=superHistory)或不使用pattern2模式，则启用两距离预取
        if (originalTrainEntry->highPatternConfidence >= superHistory || !use_pattern2)
            originalTrainEntry->currently_twodist_pf = true;
        // 如果模式置信度较低且使用pattern2模式，则禁用两距离预取
        if (originalTrainEntry->patternConfidence < upperHistory && use_pattern2)
            originalTrainEntry->currently_twodist_pf = false;

        // 如果对模式非常确信，则使用更远的历史地址作为索引
        // 这可以实现更大步长的预取，提高预取距离
        // TODO: 可以通过跟踪MSHR中的PC来学习预取时机
        if (originalTrainEntry->currently_twodist_pf && should_lookahead)
            index = originalTrainEntry->lastLastAddress;  // 使用倒数第二次访问的地址

        target = addr;  // 设置预取目标地址为当前访问地址

        // 决定是否应该进行预取：需要同时满足重用置信度和模式置信度的条件
        // 重用置信度：预测该地址是否会被重用访问
        // 模式置信度：预测访问模式的准确性 (8是重置点)
        should_pf = (originalTrainEntry->reuseConfidence > upperReuse || !use_reuse) &&
                    (originalTrainEntry->patternConfidence > upperHistory || !use_pattern);

        global_timestamp++;  // 更新全局时间戳
    }

    // 只有对于频繁访问的条目才开始添加采样
    // 检查全局置信度是否足够高，决定是否可以进行训练单元的分配
    bool high_global_conf =
        ((!use_reuse || globalReuseConfidence > 64) && (!use_pattern2 || globalHighPatternConfidence > 64) &&
         (!use_pattern || globalPatternConfidence > 64));

    // 如果在训练单元中没有找到该PC的条目，且满足以下任一条件：
    // 1. 随机概率满足条件（用于控制新条目的创建频率）
    // 2. 全局置信度足够高（表示系统整体预测能力较强）
    if (originalTrainEntry == nullptr && (randomChance(8, 8) || high_global_conf)) {
        // TODO: 是否应该使用无标签的方式？

        // 如果不是因为高全局置信度而创建条目，则标记需要采样
        // 这样可以区分不同创建原因，用于后续的采样策略
        if (!high_global_conf)
            should_sample = true;
        originalTrainEntry = trainingUnit.findVictim(pc);
        DPRINTF(TriangelDebug, "Replacing Training Entry %x\n", pc);
        assert(originalTrainEntry != nullptr);   // 确保找到了受害者条目
        assert(!originalTrainEntry->isValid());  // 确保受害者条目是无效的（已被清空）

        // 将新的PC条目插入到训练单元中
        trainingUnit.insertEntry(pc, is_secure, originalTrainEntry);

        // 如果全局高模式置信度非常高（>96），则立即启用两距离预取
        // 这是基于系统整体表现的激进优化策略
        if (globalHighPatternConfidence > 96)
            originalTrainEntry->currently_twodist_pf = true;
    }



    // 如果在 TrainUnit 命中了
    if (correlated_addr_found) {

        // 检查第二机会采样器中是否有最近的历史记录
        // 第二机会采样器用于跟踪预测失败的模式，给予它们第二次机会来证明准确性
        SecondChanceEntry *tEntry = secondChanceUnit.findEntry(addr, is_secure);
        if (tEntry != nullptr && !tEntry->used) {
            // 如果找到了未使用的条目，标记为已使用，避免重复处理
            tEntry->used = true;

            // 查找与该secondChance条目PC对应的训练单元条目
            TrainingUnitEntry *pEntry = trainingUnit.findEntry(tEntry->pc, is_secure);
            if (pEntry != nullptr) {
                // 检查时间有效性：如果启用了时间限制且条目太旧，则视为过期
                if (!timed_scs || (tEntry->global_timestamp + 512 > second_chance_timestamp)) {
                    // 如果是同一个PC触发的访问
                    // 此时tEntry的pc和addr都与当前cache访问相同
                    // 说明pEntry的代表的模式仍然有效
                    if (tEntry->pc == pc) {
                        // 增加模式置信度，奖励正确的预测
                        pEntry->patternConfidence++;
                        pEntry->highPatternConfidence++;
                        globalPatternConfidence++;
                        globalHighPatternConfidence++;
                    }
                } else {
                    // 条目已过期，说明预测失败，需要惩罚
                    pEntry->patternConfidence--;
                    globalPatternConfidence--;

                    // 如果不使用性能偏置，则额外惩罚
                    if (!perfbias) {
                        pEntry->patternConfidence--;
                        globalPatternConfidence--;
                    }  // 性能偏置：减少对错误预测的惩罚力度

                    // 对高模式置信度进行更严厉的惩罚
                    // perfbias为true时减少2次，否则减少5次
                    for (int x = 0; x < (perfbias ? 2 : 5); x++) {
                        pEntry->highPatternConfidence--;
                        globalHighPatternConfidence--;
                    }
                }
            }
        }

        // 检查历史采样器中是否存在对应的条目
        // 历史采样器用于跟踪地址的重用模式和访问间隔
        SampleEntry *sEntry = historySampler.findEntry(originalTrainEntry->lastAddress, is_secure);
        if (sEntry != nullptr && sEntry->entry == originalTrainEntry) {
            // 计算时间距离：当前训练条目的时间戳 - 采样条目创建时的时间戳
            // 这个距离反映了地址重用的时间间隔
            int64_t distance = sEntry->entry->local_timestamp - sEntry->local_timestamp;

            // 根据距离判断重用是否有效并更新重用置信度
            if (distance > 0 && distance < max_size) {
                // 距离合理，说明重用是有效的，增加重用置信度
                originalTrainEntry->reuseConfidence++;
                globalReuseConfidence++;
            } else if (!sEntry->reused) {
                // 距离过大或无效，且之前未被标记为重用，减少重用置信度
                originalTrainEntry->reuseConfidence--;
                globalReuseConfidence--;
            }
            // 标记该采样条目已被重用，避免重复处理
            sEntry->reused = true;

            DPRINTF(TriangelDebug,
                    "Found reuse for addr %x, PC %x, distance %ld (train %ld vs sample %ld) confidence %d\n", addr, pc,
                    distance, originalTrainEntry->local_timestamp, sEntry->local_timestamp,
                    originalTrainEntry->reuseConfidence + 0);

            // 检查模式预测是否准确：当前地址是否与预期的下一个地址匹配
            bool willBeConfident = addr == sEntry->next;

            // 如果预测准确，或者满足第二机会条件（目标地址在缓存中且未被预取过）
            if (willBeConfident || (use_scs && inCache(sEntry->next << lBlkSize, is_secure) &&
                                    !hasBeenPrefetched(sEntry->next << lBlkSize, is_secure))) {
                if (willBeConfident) {
                    // 预测完全正确，奖励所有模式置信度
                    originalTrainEntry->patternConfidence++;
                    originalTrainEntry->highPatternConfidence++;
                    globalPatternConfidence++;
                    globalHighPatternConfidence++;
                }
                // if (entry->replaceRate < 8) entry->replaceRate.reset();
            } else {
                // 没有发现预期的(x,y)模式，在看到y时。因此将x放入第二机会采样器
                if (use_scs) {
                    // 在第二机会单元中为预期的下一个地址创建条目
                    SecondChanceEntry *tEntry = secondChanceUnit.findVictim(sEntry->next);
                    if (tEntry->pc != 0 && !tEntry->used) {
                        // 如果要替换的条目有有效PC且未被使用，先对其进行惩罚
                        // 查找与该PC对应的训练单元条目
                        TrainingUnitEntry *pEntry = trainingUnit.findEntry(tEntry->pc, is_secure);
                        if (pEntry != nullptr) {
                            // 减少模式置信度，因为预期的模式没有匹配
                            pEntry->patternConfidence--;
                            globalPatternConfidence--;

                            // 如果不使用性能偏置，则额外减少置信度
                            if (!perfbias) {
                                pEntry->patternConfidence--;
                                globalPatternConfidence--;
                            }  // 性能偏置：减少惩罚力度

                            // 大幅减少高模式置信度，惩罚力度更大
                            // perfbias为true时减少2次，否则减少5次
                            for (int x = 0; x < (perfbias ? 2 : 5); x++) {
                                pEntry->highPatternConfidence--;
                                globalHighPatternConfidence--;
                            }
                        }
                    }
                    // 将预期地址插入第二机会单元，给予第二次验证机会
                    secondChanceUnit.insertEntry(sEntry->next, is_secure, tEntry);
                    tEntry->pc = pc;                                     // 记录触发PC
                    tEntry->global_timestamp = second_chance_timestamp;  // 记录时间戳
                    tEntry->used = false;                                // 标记为未使用
                } else {
                    // 如果不使用第二机会采样器，直接惩罚当前训练条目
                    originalTrainEntry->patternConfidence--;
                    globalPatternConfidence--;
                    if (!perfbias) {
                        originalTrainEntry->patternConfidence--;
                        globalPatternConfidence--;
                    }
                    // 对高模式置信度进行更严厉的惩罚
                    for (int x = 0; x < (perfbias ? 2 : 5); x++) {
                        originalTrainEntry->highPatternConfidence--;
                        globalHighPatternConfidence--;
                    }
                }
            }

            // 调试输出：如果地址匹配，记录匹配信息
            if (addr == sEntry->next)
                DPRINTF(TriangelDebug, "Match for address %x confidence %d\n", addr,
                        originalTrainEntry->patternConfidence + 0);

            // 更新采样条目的预测信息
            if (sEntry->entry == originalTrainEntry)
                // 如果不使用采样置信度或当前不够置信，则更新下一个预期地址
                if (!useSampleConfidence || !sEntry->confident)
                    sEntry->next = addr;
            // 更新置信度状态
            sEntry->confident = willBeConfident;
        } else if (should_sample ||
                   randomChance(originalTrainEntry->reuseConfidence, originalTrainEntry->replaceRate)) {
            // 如果需要采样或者随机机会满足条件，则填充采样表
            // should_sample 在之前的随机机会判断中设置，在首次将PC插入训练表时
            sEntry = historySampler.findVictim(originalTrainEntry->lastAddress);
            assert(sEntry != nullptr);

            // 如果即将被替换的采样条目中有有效的训练单元条目
            if (sEntry->entry != nullptr) {
                TrainingUnitEntry *pentry = sEntry->entry;
                if (pentry != nullptr) {
                    // 计算时间距离：训练条目的时间戳 - 采样条目的时间戳
                    int64_t distance = pentry->local_timestamp - sEntry->local_timestamp;
                    DPRINTF(TriangelDebug, "Replacing Entry %x with PC %x, old distance %d\n", sEntry->entry, pc,
                            distance);

                    // 如果距离太大（超过最大跟踪大小），说明数据太旧
                    if (distance > max_size) {
                        // TODO: 将最大大小改为相对值，基于当前跟踪集合？
                        trainingUnit.accessEntry(pentry);
                        if (!sEntry->reused) {
                            // 由于数据太旧，减少重用置信度
                            pentry->reuseConfidence--;
                            globalReuseConfidence--;
                        }
                        // 只替换旧条目，可以更加激进
                        originalTrainEntry->replaceRate++;
                    } else if (distance > 0 && !sEntry->reused) {
                        // 距离可能由于训练条目空间不足而变为负数
                        originalTrainEntry->replaceRate--;
                    }
                } else
                    originalTrainEntry->replaceRate++;
            }

            // 确保采样条目无效，然后清空
            assert(!sEntry->isValid());
            sEntry->clear();

            // 在历史采样器中插入新条目，使用当前训练条目的最后访问地址作为索引
            historySampler.insertEntry(originalTrainEntry->lastAddress, is_secure, sEntry);

            // 设置采样条目的各个字段
            sEntry->entry = originalTrainEntry;                                 // 关联到当前训练条目
            sEntry->reused = false;                                             // 标记为未重用
            sEntry->local_timestamp = originalTrainEntry->local_timestamp + 1;  // 设置本地时间戳
            sEntry->next = addr;                                                // 记录下一个预期访问的地址
            sEntry->confident = false;                                          // 初始置信度为假
        }
    }

    // 如果不使用布隆过滤器模式，则使用大小决斗器来动态调整缓存分区
    if (!use_bloom) {

        // 遍历所有大小决斗器（根据配置选择32个或64个）
        for (int x = 0; x < (smallduel ? 32 : 64); x++) {
            // 更新大小决斗器，确定对于每个缓存集合，使用马尔可夫表还是L3缓存更好
            // TODO: 是否可以与Hawkeye算法结合？
            int res = sizeDuelPtr[x].checkAndInsert(addr, should_pf);
            if (res == 0)
                continue;  // 如果没有命中，跳过此决斗器

            // 设置性能评估的比例参数
            const int ratioNumer = (perfbias ? 4 : 2);  // 分子：性能偏置时为4，否则为2
            const int ratioDenom = 4;                   // 分母：固定为4

            // 解码返回结果中的命中信息（使用位编码）
            int cache_hit = res % 128;      // 缓存命中的编码（取模128）
            int pref_hit = res / 128;       // 预取命中的编码（整除128）
            int cache_set = cache_hit - 1;  // 缓存命中时的替换状态位置（第几个最常用的）
            int pref_set = pref_hit - 1;    // 预取命中时的替换状态位置（第几个最常用的）

            // 验证索引的有效性
            assert(!cache_hit || (cache_set < setPrefetch.size() - 1 && cache_set >= 0));
            assert(!pref_hit || (pref_set < setPrefetch.size() - 1 && pref_set >= 0));

            // 处理缓存命中的情况
            if (cache_hit)
                // 对于所有大于等于当前大小的分区配置，增加其命中计数
                // 例如：在way 14命中 = y=17-2-14=1和0：表示在预留0或1个way时都会命中，但不是2个
                for (int y = setPrefetch.size() - 2 - cache_set; y >= 0; y--)
                    setPrefetch[y]++;

            // 处理预取命中的情况
            if (pref_hit)
                // 对于所有大于等于当前大小的预取分区配置，增加其加权命中计数
                // 使用一索引（因为0表示分配0个way）。在way 0命中 = y=1--16个way预留，而不是0个
                for (int y = pref_set + 1; y < setPrefetch.size(); y++)
                    setPrefetch[y] += (ratioNumer * sizeDuelPtr[x].temporalModMax) / ratioDenom;

            // 调试信息（已注释）
            // if (cache_hit) printf("Cache hit\n");
            // else printf("Prefetch hit\n");
        }


        // 当全局时间戳超过50万时，开始一个重新调整马尔可夫和cache容量
        if (global_timestamp > 500000) {
            // 根据上一个纪元的最优表现选择马尔可夫表的大小
            int counterSizeSeen = 0;

            // 遍历所有可能的预取分区大小，找到表现最好的配置
            for (int x = 0; x < setPrefetch.size() && x * size_increment <= max_size; x++) {
                if (setPrefetch[x] > counterSizeSeen) {
                    target_size = size_increment * x;  // 设置目标大小
                    counterSizeSeen = setPrefetch[x];  // 记录最高分数
                }
            }

            // 获取当前配置的性能分数
            int currentscore = setPrefetch[current_size / size_increment];
            // 为避免微小收益导致的频繁切换，给当前配置加一点偏置
            currentscore = currentscore + (currentscore >> 4);
            int targetscore = setPrefetch[target_size / size_increment];

            // 如果目标大小不同于当前大小，且目标配置的分数确实更高，则进行切换
            if (target_size != current_size && targetscore > currentscore) {
                current_size = target_size;
                printf("size: %d, tick %ld \n", current_size, curTick());
                // 调试输出（已注释）
                // for (int x=0;x<setPrefetch.size(); x++) {
                //	printf("%d: %d\n", x, setPrefetch[x]);
                // }
                assert(current_size >= 0);

                // 重新配置所有HawkEye集合的掩码和状态
                for (int x = 0; x < 64; x++) {
                    hawksets[x].setMask = current_size / hawksets[x].maxElems;
                    hawksets[x].reset();
                }

                // 准备重新排列马尔可夫表中的条目
                std::vector<MarkovMapping> ams;
                if (should_rearrange) {
                    // 保存所有有效的马尔可夫映射条目
                    for (MarkovMapping am : *markovTablePtr) {
                        if (am.isValid())
                            ams.push_back(am);
                    }
                    // 清空所有条目（为RRIP替换策略做准备）
                    for (MarkovMapping &am : *markovTablePtr) {
                        am.invalidate();
                    }
                }

                // 更新哈希集合关联度配置
                TriangelHashedSetAssociative *thsa =
                    dynamic_cast<TriangelHashedSetAssociative *>(markovTablePtr->indexingPolicy);
                if (thsa) {
                    thsa->ways = current_size / size_increment;  // 设置新的关联度
                    thsa->max_ways = maxWays;                    // 设置最大关联度
                    assert(thsa->ways <= thsa->max_ways);
                } else
                    assert(0);

                // 有条件地重新排列马尔可夫表条目
                if (should_rearrange) {
                    if (current_size > 0) {
                        // 将之前保存的有效条目重新插入到调整后的表中
                        for (MarkovMapping am : ams) {
                            MarkovMapping *mapping = getHistoryEntry(am.index, am.isSecure(), true, false, true, true);
                            mapping->address = am.address;
                            mapping->index = am.index;
                            mapping->confident = am.confident;
                            mapping->lookupIndex = am.lookupIndex;
                            markovTablePtr->weightedAccessEntry(mapping, 1, false);  // 为RRIP策略进行触碰
                        }
                    }
                }

                // 使超出新配置范围的条目失效
                for (MarkovMapping &am : *markovTablePtr) {
                    // set 的低位指明了该set落在哪个cache way
                    if (thsa->ways == 0 || (thsa->extractSet(am.index) % maxWays) >= thsa->ways)
                        am.invalidate();
                }
                // 调整缓存标签的最大分配路数
                cachetags->setWayAllocationMax(setPrefetch.size() - 1 - thsa->ways);
            }

            // 输出纪元结束的统计信息
            printf("End of epoch:\n");
            for (int x = 0; x < setPrefetch.size(); x++) {
                printf("%d: %d\n", x, setPrefetch[x]);
            }

            // 重置计数器，开始新的纪元
            global_timestamp = 0;
            for (int x = 0; x < setPrefetch.size(); x++) {
                setPrefetch[x] = 0;
            }
            // 在200万次预取访问后重置 -- 虽然不完全等同于3000万条指令，但足够接近
        }
    }


    // HawkEye算法：用于缓存友好性预测和替换策略优化
    if (useHawkeye && correlated_addr_found && should_pf) {
        // 如果找到了地址关联且应该进行预取，则更新马尔可夫表
        // HawkEye算法通过跟踪访问模式来预测缓存行的未来重用情况
        for (int x = 0; x < 64; x++)
            // 将当前访问的地址和PC添加到所有HawkEye集合中进行学习
            // 这些集合用于训练HawkEye的缓存友好性预测模型
            hawksets[x].add(addr, pc, &trainingUnit);

        // 根据训练单元中的HawkEye置信度决定是否启用HawkEye策略
        // 置信度阈值为7，超过此值表示HawkEye预测较为可靠
        should_hawk = originalTrainEntry->hawkConfidence > 7;
    }

    if (use_bloom) {
        if (correlated_addr_found && should_pf) {
            if (bloomset == -1)
                bloomset = index & 127;
            if ((index & 127) == bloomset) {
                int add = bloom_add(blptr, &index, sizeof(Addr));
                if (!add) {
                    target_size += 192;

                    // printf("Bloom: pc %ld conf %d %d %d rate %d\n", pc,
                    // entry->reuseConfidence+0,entry->patternConfidence+0,
                    // entry->highPatternConfidence+0,entry->replaceRate+0);
                }
            }
        }

        while (target_size > current_size && target_size > size_increment / 8 && current_size < max_size) {
            // check for size_increment to leave empty if unlikely to be useful.
            current_size += size_increment;
            printf("size: %d, tick %ld \n", current_size, curTick());
            assert(current_size <= max_size);
            assert(cachetags->getWayAllocationMax() > 1);
            cachetags->setWayAllocationMax(cachetags->getWayAllocationMax() - 1);

            std::vector<MarkovMapping> ams;
            if (should_rearrange) {
                for (MarkovMapping am : *markovTablePtr) {
                    if (am.isValid())
                        ams.push_back(am);
                }
                for (MarkovMapping &am : *markovTablePtr) {
                    am.invalidate();  // for RRIP's sake
                }
            }
            TriangelHashedSetAssociative *thsa =
                dynamic_cast<TriangelHashedSetAssociative *>(markovTablePtr->indexingPolicy);
            if (thsa) {
                thsa->ways++;
                thsa->max_ways = maxWays;
                assert(thsa->ways <= thsa->max_ways);
            } else
                assert(0);
            // TODO: rearrange conditionally
            if (should_rearrange) {
                for (MarkovMapping am : ams) {
                    MarkovMapping *mapping = getHistoryEntry(am.index, am.isSecure(), true, false, true, true);
                    mapping->address = am.address;
                    mapping->index = am.index;
                    mapping->confident = am.confident;
                    mapping->lookupIndex = am.lookupIndex;
                    markovTablePtr->weightedAccessEntry(mapping, 1, false);  // For RRIP, touch
                }
            }
            // increase associativity of the set structure by 1!
            // Also, decrease LLC cache associativity by 1.
        }

        if (global_timestamp > 2000000) {
            // Reset after 2 million prefetch accesses -- not quite the same as after 30 million insts but close enough

            while ((target_size <= current_size - size_increment || target_size < size_increment / 8) &&
                   current_size >= size_increment) {
                // reduce the assoc by 1.
                // Also, increase LLC cache associativity by 1.
                current_size -= size_increment;
                printf("size: %d, tick %ld \n", current_size, curTick());
                assert(current_size >= 0);
                std::vector<MarkovMapping> ams;
                if (should_rearrange) {
                    for (MarkovMapping am : *markovTablePtr) {
                        if (am.isValid())
                            ams.push_back(am);
                    }
                    for (MarkovMapping &am : *markovTablePtr) {
                        am.invalidate();  // for RRIP's sake
                    }
                }
                TriangelHashedSetAssociative *thsa =
                    dynamic_cast<TriangelHashedSetAssociative *>(markovTablePtr->indexingPolicy);
                if (thsa) {
                    assert(thsa->ways > 0);
                    thsa->ways--;
                } else
                    assert(0);
                // rearrange conditionally
                if (should_rearrange) {
                    if (current_size > 0) {
                        for (MarkovMapping am : ams) {
                            MarkovMapping *mapping = getHistoryEntry(am.index, am.isSecure(), true, false, true, true);
                            mapping->address = am.address;
                            mapping->index = am.index;
                            mapping->confident = am.confident;
                            mapping->lookupIndex = am.lookupIndex;
                            markovTablePtr->weightedAccessEntry(mapping, 1, false);  // For RRIP, touch
                        }
                    }
                }

                for (MarkovMapping &am : *markovTablePtr) {
                    if (thsa->ways == 0 || (thsa->extractSet(am.index) % maxWays) >= thsa->ways)
                        am.invalidate();
                }




                cachetags->setWayAllocationMax(cachetags->getWayAllocationMax() + 1);
            }
            target_size = 0;
            global_timestamp = 0;
            bloom_reset(blptr);
            bloomset = -1;
        }
    }


    // 如果找到了地址相关性且满足预取条件，并且Markov表容量大于0，更新Markov表
    if (correlated_addr_found && should_pf && (current_size > 0)) {
        // 1. 查找或分配 MarkovMapping 条目（马尔可夫映射表），先尝试只读查找，如果没有则分配新条目并初始化
        MarkovMapping *mapping = getHistoryEntry(index, is_secure, false, false, false, should_hawk);
        if (mapping == nullptr) {
            // 没有找到则分配新条目，并初始化目标地址、索引、置信度
            mapping = getHistoryEntry(index, is_secure, true, false, false, should_hawk);
            mapping->address = target;   // 记录本次预取的目标地址
            mapping->index = index;      // 记录索引（HawkEye算法用）
            mapping->confident = false;  // 初始置信度为假
        }
        assert(mapping != nullptr);
        // 2. 判断当前mapping是否与目标一致，更新置信度
        bool confident = mapping->address == target;
        bool wasConfident = mapping->confident;
        mapping->confident = confident;  // 置信度仅用于替换策略
        if (!wasConfident) {
            // 如果之前不置信，则更新目标地址
            mapping->address = target;
        }
        // 3. 如果之前已经置信且仍然置信，并且启用MRB，则减少一次元数据访问统计
        if (wasConfident && confident && use_mrb) {
            MarkovMapping *cached_entry = metadataReuseBuffer.findEntry(index, is_secure);
            if (cached_entry != nullptr) {
                triangelStats.metadataAccesses--;
                // 不需要再次访问L3，直接命中
            }
        }

        // 4. 可选：更新lookupTable（用于多路查找优化）
        int index = 0;
        uint64_t time = -1;
        if (lookupAssoc > 0) {
            int lookupMask = (1024 / lookupAssoc) - 1;
            int set = (target >> lookupOffset) & lookupMask;
            // 在lookupTable的对应集合中查找目标
            for (int x = lookupAssoc * set; x < lookupAssoc * (set + 1); x++) {
                if (target >> lookupOffset == lookupTable[x]) {
                    index = x;
                    break;
                }
                if (time > lookupTick[x]) {
                    time = lookupTick[x];
                    index = x;
                }
            }

            // 更新lookupTable和时间戳
            lookupTable[index] = target >> lookupOffset;
            lookupTick[index] = curTick();
            mapping->lookupIndex = index;
        }
    }

    // 多级预取循环：根据MarkovMapping链表递归发起多步预取
    if (target != 0 && should_pf && (current_size > 0)) {
        // 1. 以target为起点，查找MarkovMapping链表，递归发起多级预取
        MarkovMapping *pf_target = getHistoryEntry(target, is_secure, false, true, false, should_hawk);
        unsigned deg = 0;             // 当前预取深度
        unsigned delay = cacheDelay;  // 预取延迟
        // 判断是否允许高阶多级预取
        bool high_degree_pf =
            pf_target != nullptr && (originalTrainEntry->highPatternConfidence > highUpperHistory || !use_pattern2);
        unsigned max = high_degree_pf ? degree : (should_pf ? 1 : 0);  // 最大递归深度

        // 2. 递归发起多级预取，直到达到最大深度或链表断裂
        while (pf_target != nullptr && deg < max) {
            // 调试输出：实际发起的预取地址
            DPRINTF(TriangelDebug, "Prefetching %x on miss at %x, PC %x\n", pf_target->address << lBlkSize,
                    addr << lBlkSize, pc);
            int extraDelay = cacheDelay;
            // 如果上次访问来自MRB且启用MRB，动态调整延迟
            if (lastAccessFromPFCache && use_mrb) {
                Cycles time = curCycle() - pf_target->cycle_issued;
                if (time >= cacheDelay)
                    extraDelay = 0;
                else if (time < cacheDelay)
                    extraDelay = cacheDelay - time;
            }

            Addr lookup = pf_target->address;
            // 3. 如果启用lookupAssoc，使用lookupTable优化实际预取地址
            if (lookupAssoc > 0) {
                int index = pf_target->lookupIndex;
                int lookupMask = (1 << lookupOffset) - 1;
                lookup = (lookupTable[index] << lookupOffset) + ((pf_target->address) & lookupMask);
                lookupTick[index] = curTick();
                if (lookup == pf_target->address)
                    triangelStats.lookupCorrect++;
                else
                    triangelStats.lookupWrong++;
            }

            // 4. 记录本次预取请求
            if (extraDelay == cacheDelay)
                // addresses.push_back(AddrPriority(lookup << lBlkSize, delay));
                addresses.push_back(AddrPriority(lookup << lBlkSize, 32 - deg, PrefetchSourceType::Triangel));
            delay += extraDelay;
            deg++;

            // 5. 递归查找下一个MarkovMapping，实现多级链式预取
            if (deg < max)
                pf_target = getHistoryEntry(lookup, is_secure, false, true, false, should_hawk);
            else
                pf_target = nullptr;
        }
    }

    // Update the entry
    if (originalTrainEntry != nullptr) {
        originalTrainEntry->lastLastAddress = originalTrainEntry->lastAddress;
        originalTrainEntry->lastLastAddressSecure = originalTrainEntry->lastAddressSecure;
        originalTrainEntry->lastAddress = addr;
        originalTrainEntry->lastAddressSecure = is_secure;
        originalTrainEntry->local_timestamp++;
    }
}

Triangel::MarkovMapping *
Triangel::getHistoryEntry(Addr index, bool is_secure, bool replace, bool readonly, bool clearing, bool hawk)
{
    // 上述这些奇怪的参数控制是否替换条目，以及如何更新元数据访问次数等
    // 它们基本上是模拟相关的配置参数
    TriangelHashedSetAssociative *thsa = dynamic_cast<TriangelHashedSetAssociative *>(markovTablePtr->indexingPolicy);
    if (!thsa)
        assert(0);

    // 清除缓存标签中对应的集合和路，为马尔可夫表的访问做准备
    // 这是为了模拟真实硬件中缓存和预取器元数据表之间的交互
    cachetags->clearSetWay(thsa->extractSet(index) / maxWays, thsa->extractSet(index) % maxWays);

    // 如果启用了重新排列功能，则跟踪不同索引的关联度变化
    // 用于统计元数据访问次数，这对性能分析很重要
    if (should_rearrange) {
        // 使用简化的索引策略，虽然不完全相同，但足够接近
        int _index = index % (way_idx.size());

        // 如果该索引的关联度发生了变化，需要更新统计信息
        if (way_idx[_index] != thsa->ways) {
            if (way_idx[_index] != 0)
                // 累加新旧关联度的访问开销
                triangelStats.metadataAccesses += thsa->ways + way_idx[_index];
            way_idx[_index] = thsa->ways;  // 更新为当前关联度
        }
    }

    // 如果是只读访问，首先检查元数据重用缓冲区（MRB）
    // MRB用于缓存最近访问的元数据，减少对主存储器的访问
    if (readonly) {
        MarkovMapping *pf_entry = use_mrb ? metadataReuseBuffer.findEntry(index, is_secure) : nullptr;
        if (pf_entry != nullptr) {
            // 在预取缓存中找到了条目，设置标志并直接返回
            lastAccessFromPFCache = true;
            return pf_entry;
        }
        lastAccessFromPFCache = false;  // 未在预取缓存中找到
    }

    // 在主马尔可夫表中查找条目
    MarkovMapping *ps_entry = markovTablePtr->findEntry(index, is_secure);

    // 如果是只读访问或不添加新条目，则统计元数据访问次数
    if (readonly || !replace)
        triangelStats.metadataAccesses++;

    if (ps_entry != nullptr) {
        // 找到了现有的PS-AMC（Prefetch Storage - Address Mapping Cache）条目
        // 根据HawkEye算法的权重更新访问记录
        markovTablePtr->weightedAccessEntry(ps_entry, hawk ? 1 : 0, false);
    } else {
        // 没有找到条目
        if (!replace)
            return nullptr;  // 如果不允许添加新条目，直接返回空指针

        // 需要添加新条目，首先找到一个受害者条目进行替换
        ps_entry = markovTablePtr->findVictim(index);
        assert(ps_entry != nullptr);

        // 如果使用HawkEye算法且不是清理操作，需要更新HawkEye的LRU信息
        if (useHawkeye && !clearing)
            for (int x = 0; x < 64; x++)
                // 通知所有HawkEye集合该条目即将被LRU替换
                hawksets[x].decrementOnLRU(ps_entry->index, &trainingUnit);

        assert(!ps_entry->isValid());  // 确保受害者条目是无效的

        // 插入新条目到马尔可夫表中
        markovTablePtr->insertEntry(index, is_secure, ps_entry);
        // 根据HawkEye权重更新访问记录，标记为新插入
        markovTablePtr->weightedAccessEntry(ps_entry, hawk ? 1 : 0, true);
    }

    // 如果是只读访问且使用元数据重用缓冲区，则将条目缓存到MRB中
    if (readonly && use_mrb) {
        // 在MRB中找一个受害者位置
        MarkovMapping *pf_entry = metadataReuseBuffer.findVictim(index);
        metadataReuseBuffer.insertEntry(index, is_secure, pf_entry);

        // 将主表中的数据复制到MRB缓存中
        pf_entry->address = ps_entry->address;
        pf_entry->confident = ps_entry->confident;
        pf_entry->cycle_issued = curCycle();  // 记录访问时间，用于适当设置延迟
    }

    return ps_entry;  // 返回找到或创建的马尔可夫映射条目
}

Triangel::TriangelStats::TriangelStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(metadataAccesses, statistics::units::Count::get(), "accesses to history table in cache"),
      ADD_STAT(lookupCorrect, statistics::units::Count::get(), "lookup access correct"),
      ADD_STAT(lookupWrong, statistics::units::Count::get(), "lookup access wrong"),
      ADD_STAT(lookupCancelled, statistics::units::Count::get(), "lookup access wrong (detected)")
{
}



uint32_t
TriangelHashedSetAssociative::extractSet(const Addr addr) const
{
    // 输入已经是块索引，无需再次移除块偏移
    // Input is already blockIndex so no need to remove block again.
    Addr offset = addr;

    /* 注释掉的哈希函数实现：
     * 这些是备选的哈希策略，可以用于更复杂的地址映射
     * const Addr hash1 = offset & ((1<<16)-1);      // 取低16位
     * const Addr hash2 = (offset >> 16) & ((1<<16)-1);  // 取中间16位
     * const Addr hash3 = (offset >> 32) & ((1<<16)-1);  // 取高16位
     */
    /* const Addr hash1 = offset & ((1<<16)-1);
     const Addr hash2 = (offset >> 16) & ((1<<16)-1);
         const Addr hash3 = (offset >> 32) & ((1<<16)-1);
     */

    // 计算哈希集合索引：
    // 1. offset * max_ways: 将地址乘以最大关联度，增加地址空间的分散性
    // 2. extractTag(addr) % ways: 使用标签的哈希值模当前关联度，增加随机性
    // 3. 两者相加形成复合哈希函数，提高缓存集合分布的均匀性
    offset = ((offset)*max_ways) + (extractTag(addr) % ways);

    // 应用集合掩码获得最终的集合索引
    // setMask = numSets-1，用于将哈希值限制在有效的集合范围内
    return offset & setMask;  // setMask is numSets-1
}


Addr
TriangelHashedSetAssociative::extractTag(const Addr addr) const
{
    // 输入已经是块索引，无需再次移除块偏移
    // Input is already blockIndex so no need to remove block again.

    // Triage-ISR论文中的描述存在歧义：
    // 不清楚索引是仅使用16个最低有效位，还是使用上面的复杂索引方式
    // 如果使用字面表示，标签不能是剩余的位！
    // Description in Triage-ISR confuses whether the index is just the 16 least significant bits,
    // or the weird index above. The tag can't be the remaining bits if we use the literal representation!

    // 第一步：计算基础偏移量
    // 将地址除以（集合数/最大关联度），这样可以将地址空间进行分段
    // 这种分段方式有助于在不同的集合分区之间分布标签
    // numSet = 16K, max_ways = 8
    // hashTag = BlockIdx[20:11] ^ BlockIdx[30:21] ^ BlockIdx[40:31] ^ BlockIdx[50:41] ^ BlockIdx[60:51] ^
    // BlockIdx[63:61]

    Addr offset = addr / (numSets / max_ways);
    int result = 0;

    // 这是按照Triangel论文中描述的标签计算方法
    // This is a tag# as described in the Triangel paper.
    const int shiftwidth = 10;  // 每次处理10位，这是一个经验值

    // 使用XOR折叠技术压缩高位地址信息：
    // 将64位地址按10位为单位进行分段，然后通过异或操作将所有段压缩成一个标签
    // 这种方法可以保留地址的高位信息，同时将其压缩到较小的标签空间中
    for (int x = 0; x < 64; x += shiftwidth) {
        // 1. 提取当前10位：offset & ((1 << shiftwidth) - 1)
        // 2. 与之前的结果进行异或：result ^= ...
        // 3. 右移10位处理下一个段：offset = offset >> shiftwidth
        result ^= (offset & ((1 << shiftwidth) - 1));
        offset = offset >> shiftwidth;
    }

    // 返回计算得到的标签值
    // 这个标签将用于 extractSet() 函数中的哈希计算
    return result;
}



}  // namespace prefetch
}  // namespace gem5
