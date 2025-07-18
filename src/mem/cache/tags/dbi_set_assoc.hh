#ifndef __MEM_CACHE_TAGS_DBI_SET_ASSOC_HH__
#define __MEM_CACHE_TAGS_DBI_SET_ASSOC_HH__

#include <cstdint>
#include <string>
#include <vector>

#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
    #define USE_PEXT_IMPL 1
    #include <immintrin.h>
#else
    #define USE_PEXT_IMPL 0
#endif

#include "base/statistics.hh"
#include "mem/cache/tags/base_set_assoc.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "mem/packet.hh"
#include "params/DBISetAssoc.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{
class Base;
}
class ReplaceableEntry;

/**
 * A DBISetAssoc cache tag store.
 * @sa  \ref gem5MemorySystem "gem5 Memory System"
 *
 * The DBISetAssoc placement policy divides the cache into s sectors of w
 * consecutive sectors (ways). Each sector then consists of a number of
 * sequential cache lines that may or may not be present.
 */
class DBISetAssoc : public BaseSetAssoc
{
  private:
    const Addr rowMask;
    const Addr colMask;
	const Addr lblkSize;
    const uint16_t granularity;

  protected:
	struct DirtyBlock {
		Addr col;
		CacheBlk* blk;

		DirtyBlock(Addr c, CacheBlk* b) : col(c), blk(b) {}
	};

	struct DBIEntry : public TaggedEntry
	{
		/**
		 * Maximum number of dirty blocks allowed in this entry.
		 */
		const uint16_t maxEntries;

		/**
		 * Row address of this entry.
		 */
		Addr row;

		/**
		 * Hash map from column ID to its position in the LRU list.
		 */
		std::unordered_map<Addr, std::list<DirtyBlock>::iterator> its;

		/**
		 * Doubly-linked list maintaining LRU order (front = LRU, back = MRU).
		 */
		std::list<DirtyBlock> dirty_blks;

		DBIEntry(const uint16_t& entries)
			: TaggedEntry(), maxEntries(entries)
		{}

		~DBIEntry() {
			invalidateAll(); // Clean up all entries
		}

		/**
		 * Insert a new dirty block based on column address.
		 * Promote to MRU if it already exists.
		 * Evict the LRU block if capacity is full.
		 */
		void insertEntry(Addr col, CacheBlk* blk) {
			auto it = its.find(col);
			if (it != its.end()) {
				// Promote to MRU
				dirty_blks.splice(dirty_blks.end(), dirty_blks, it->second);
			} else {
				// Evict LRU if full
				if (dirty_blks.size() >= maxEntries) {
					auto victim_it = dirty_blks.begin();
					Addr victim_col = victim_it->col;
					its.erase(victim_col);
					dirty_blks.erase(victim_it);
				}

				// Insert new block at MRU position
				dirty_blks.emplace_back(col, blk);
				its[col] = std::prev(dirty_blks.end());
			}
		}

		/**
		 * Promote the block with the given column to MRU.
		 */
		void touchEntry(Addr col) {
			auto it = its.find(col);
			if (it != its.end()) {
				dirty_blks.splice(dirty_blks.end(), dirty_blks, it->second);
			}
		}

		/**
		 * Return the least recently used (LRU) dirty block, if any.
		 */
		DirtyBlock* findVictim() {
			if (dirty_blks.empty()) return nullptr;
			return &dirty_blks.front();
		}

		/**
		 * Check if a dirty block exists for the given column.
		 */
		bool containEntry(Addr col) const {
			return its.find(col) != its.end();
		}

		/**
		 * Find and return the dirty block for the given column, or nullptr if not found.
		 */
		DirtyBlock* findEntry(Addr col) const {
			auto it = its.find(col);
			if (it != its.end()) {
				return const_cast<DirtyBlock*>(&*(it->second));
			}
			return nullptr;
		}

		/**
		 * Erase the dirty block corresponding to the given column.
		 */
		void eraseEntry(Addr col) {
			auto it = its.find(col);
			if (it != its.end()) {
				dirty_blks.erase(it->second);
				its.erase(it);
			}
		}

		/**
		 * Invalidate and remove all dirty blocks.
		 */
		void invalidateAll() {
			row = 0;
			dirty_blks.clear();
			its.clear();
			invalidate();
		}
	};

    AssociativeSet<DBIEntry> meta;

    inline uint64_t extract_by_mask64(uint64_t data, uint64_t mask) {
    #if USE_PEXT_IMPL
        return _pext_u64(data, mask);
    #else
        uint64_t result = 0;
        int out_bit = 0;
        for (int i = 0; i < 64; ++i) {
            if ((mask >> i) & 1) {
                if ((data >> i) & 1) {
                    result |= (1ULL << out_bit);
                }
                ++out_bit;
            }
        }
        return result;
    #endif
    }

    Addr rowAddress(Addr addr) {
      return extract_by_mask64(addr >> lblkSize, rowMask);
    }

    Addr colAddress(Addr addr) {
      return extract_by_mask64(addr >> lblkSize, colMask);
    }

  public:
    /**
     * Construct and initialize this tag store.
     */
    DBISetAssoc(const DBISetAssocParams &p);

    /**
     * Destructor.
     */
    virtual ~DBISetAssoc() {};

    /**
     * This function updates the tags when a block is invalidated but does
     * not invalidate the block itself. It also updates the replacement data.
     *
     * @param blk The block to invalidate.
     */
    void invalidate(CacheBlk *blk) override;

    /**
     * Access block and update replacement data. May not succeed, in which case
     * nullptr is returned. This has all the implications of a cache access and
     * should only be used as such. Returns the tag lookup latency as a side
     * effect.
     *
     * @param pkt The packet holding the address to find.
     * @param lat The latency of the tag lookup.
     * @return Pointer to the cache block if found.
     */
    CacheBlk* accessBlock(const PacketPtr pkt, Cycles &lat) override;
    
    /**
     * Insert the new block into the cache and update replacement data.
     *
     * @param pkt Packet holding the address to update
     * @param blk The block to update.
     */
    void insertBlock(const PacketPtr pkt, CacheBlk *blk) override;

    /**
     * Find replacement victim based on packet.
     *
     * @param pkt The packet holding the address to find a victim for.
     * @param is_secure True if the target memory space is secure.
     * @param size Size, in bits, of new block to allocate.
     * @param evict_blks Cache blocks to be evicted.
     * @param clean_blks Cache blocks to be written back.
     * @param ready_to_clean True if the blocks is ready to be cleaned 
     * @return Cache block to be replaced.
     */
    CacheBlk* findVictim(PacketPtr pkt, const bool is_secure,
						 const std::size_t size,
						 std::vector<CacheBlk*>& evict_blks,
						 std::vector<CacheBlk*>& clean_blks,
						 const std::size_t& num_wb_entries) override;
      
};

}  // namespace gem5

#endif  //__MEM_CACHE_TAGS_DBI_SET_ASSOC_HH__
