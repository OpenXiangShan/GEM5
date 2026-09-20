/*
 * Copyright (c) 2026 OpenXiangShan
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

#ifndef __MEM_CACHE_PARTIAL_LINE_META_HH__
#define __MEM_CACHE_PARTIAL_LINE_META_HH__

#include <cstddef>
#include <list>
#include <unordered_map>
#include <vector>

namespace gem5
{

class CacheBlk;

/**
 * A bounded LRU directory for partial cache blocks.
 *
 * CacheBlk owns the byte-valid mask. This table only bounds the number of
 * partial blocks allowed to reside in the cache and selects a block to evict
 * when that bound is reached.
 */
class PartialLineMetaTable
{
  private:
    using LruList = std::list<CacheBlk *>;

    const size_t maxEntries;
    LruList lru;
    std::unordered_map<const CacheBlk *, LruList::iterator> entries;

  public:
    explicit PartialLineMetaTable(size_t max_entries);

    size_t capacity() const { return maxEntries; }
    size_t size() const { return entries.size(); }
    bool empty() const { return entries.empty(); }
    bool full() const { return size() >= capacity(); }

    bool contains(const CacheBlk *blk) const;
    void insert(CacheBlk *blk);
    void touch(CacheBlk *blk);
    void erase(CacheBlk *blk);

    /** Return tracked blocks from least to most recently used. */
    std::vector<CacheBlk *> oldestFirst() const;
};

} // namespace gem5

#endif // __MEM_CACHE_PARTIAL_LINE_META_HH__
