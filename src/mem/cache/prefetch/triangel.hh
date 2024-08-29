/**
 * @file
 * Describes a history prefetcher.
 */

#ifndef __MEM_CACHE_PREFETCH_TRIANGEL_HH__
#define __MEM_CACHE_PREFETCH_TRIANGEL_HH__

#include <string>
#include <unordered_map>
#include <vector>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "mem/cache/tags/base.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/indexing_policies/set_associative.hh"
#include "mem/packet.hh"
#include "base/random.hh"

#include "params/TriangelHashedSetAssociative.hh"

#include "bloom.h"



namespace gem5
{

struct TriangelPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

/**
 * Override the default set associative to apply a specific hash function
 * when extracting a set.
 */
class TriangelHashedSetAssociative : public SetAssociative
{
  public:
    uint32_t extractSet(const Addr addr) const override;
    Addr extractTag(const Addr addr) const override;

  public:
    int ways;
    int max_ways;
    TriangelHashedSetAssociative(
        const TriangelHashedSetAssociativeParams &p)
      : SetAssociative(p), ways(0),max_ways(8)
    {
    }
    ~TriangelHashedSetAssociative() = default;
};



class Triangel : public Queued
{

    /** Number of maximum prefetches requests created when predicting */
    const unsigned degree;

    /**
     * Training Unit Entry datatype, it holds the last accessed address and
     * its secure flag
     */

    BaseTags* cachetags;
    const unsigned cacheDelay;
    const bool should_lookahead;
    const bool should_rearrange;
    
    const bool use_scs;
    const bool use_bloom;
    const bool use_reuse;
    const bool use_pattern;
    const bool use_pattern2;
    const bool use_mrb;
    const bool perfbias;
    const bool smallduel;
    const bool timed_scs;
    const bool useSampleConfidence;
    
    BaseTags* sctags;

    bool randomChance(int r, int s);
    const int max_size;
    const int size_increment;
    static int64_t global_timestamp;
    uint64_t second_chance_timestamp;
    uint64_t lowest_blocked_entry;
    static int current_size;
    static int target_size;
    const int maxWays;    
    
    static bloom* blptr;

    bloom bl;
    int bloomset=-1;
    
    std::vector<int> way_idx;
    
        SatCounter8  globalReuseConfidence;
        SatCounter8  globalPatternConfidence;
        SatCounter8 globalHighPatternConfidence;    
   

    struct TrainingUnitEntry : public TaggedEntry
    {
        Addr lastAddress;
        Addr lastLastAddress;
        int64_t local_timestamp;
        SatCounter8  reuseConfidence;
        SatCounter8  patternConfidence;
        SatCounter8 highPatternConfidence;
        SatCounter8 replaceRate;
        SatCounter8 hawkConfidence;
        bool lastAddressSecure;
        bool lastLastAddressSecure;
        bool currently_twodist_pf;



        TrainingUnitEntry() : lastAddress(0), lastLastAddress(0), local_timestamp(0),reuseConfidence(4,8), patternConfidence(4,8), highPatternConfidence(4,8), replaceRate(4,8), hawkConfidence(4,8), lastAddressSecure(false), lastLastAddressSecure(false),currently_twodist_pf(false)
        {}

        void
        invalidate() override
        {
        	TaggedEntry::invalidate();
                lastAddress = 0;
                lastLastAddress = 0;
                //local_timestamp=0; //Don't reset this, to handle replacement and still give contiguity of timestamp
                reuseConfidence.reset();
                patternConfidence.reset();
                highPatternConfidence.reset();
                replaceRate.reset();
                currently_twodist_pf = false;
                
        }
    };
    /** Map of PCs to Training unit entries */
    AssociativeSet<TrainingUnitEntry> trainingUnit;
    
    Addr lookupTable[1024];
    uint64_t lookupTick[1024];
    const int lookupAssoc;
    const int lookupOffset;
    
   
  static std::vector<uint32_t> setPrefetch; 
 
public:
  struct SizeDuel
  {
  	int idx;
  	uint64_t set;
  	uint64_t setMask;
  	uint64_t temporalMod; //[0..12] Entries Per Line
  	
  	uint64_t temporalModMax; //12 by default
  	uint64_t cacheMaxAssoc;
  	
  	
  	std::vector<Addr> cacheAddrs; // [0..16] should be set by the nsets of the L3 cache.
  	std::vector<uint64_t> cacheAddrTick;
  	std::vector<Addr> temporalAddrs;
  	std::vector<uint64_t> temporalAddrTick;
  	std::vector<bool> inserted;

  	SizeDuel()
        {
        }
        void reset(uint64_t mask, uint64_t modMax, uint64_t cacheAssoc) {
        	setMask=mask;
        	temporalModMax = modMax;
        	cacheMaxAssoc=cacheAssoc;
        	cacheAddrTick.resize(cacheMaxAssoc);
        	temporalAddrs.resize(cacheMaxAssoc);
        	cacheAddrs.resize(cacheMaxAssoc);
        	temporalAddrTick.resize(cacheMaxAssoc);
        	inserted.resize(cacheMaxAssoc,false);
        	for(int x=0;x<cacheMaxAssoc;x++) {
			cacheAddrTick[x]=0;
			temporalAddrs[x]=0;
			cacheAddrs[x]=0;
			temporalAddrTick[x]=0;
		}
		set = random_mt.random<uint64_t>(0,setMask);
		temporalMod = random_mt.random<uint64_t>(0,modMax-1); // N-1, as range is inclusive.	
        }
  	
  	int checkAndInsert(Addr addr, bool should_pf) {
	  	int ret = 0;
	  	bool foundInCache=false;
	  	bool foundInTemp=false;
	  	if((addr & setMask) != set) return ret;
  		for(int x=0;x<cacheMaxAssoc;x++) {
  			if(addr == cacheAddrs[x]) {
  				foundInCache=true; 
	  				int index=cacheMaxAssoc-1;
	  				for(int y=0;y<cacheMaxAssoc;y++) {
	  					if(cacheAddrTick[x]>cacheAddrTick[y]) index--;
	  					assert(index>=0);
	  				}  
	  				cacheAddrTick[x] = curTick();
	  				ret += index+1;	
  			}
  			if(should_pf && addr == temporalAddrs[x]) {
  				
  				
  				foundInTemp=true;
  				
	  				int index=cacheMaxAssoc-1;
	  				for(int y=0;y<cacheMaxAssoc;y++) {
	  					if(temporalAddrTick[x]>temporalAddrTick[y]) index--;
	  					assert(index>=0);
	  				}  

	  				ret += 128*(index+1);
	  			
	  			temporalAddrTick[x] = curTick();
	  			inserted[x]=true;
	  		}
  		}
  		if(!foundInCache) {
  			uint64_t oldestTick = (uint64_t)-1;
  			int idx = -1;
  			for(int x=0; x<cacheMaxAssoc;x++) {
  				if(cacheAddrTick[x]<oldestTick) {idx = x; oldestTick = cacheAddrTick[x];}
  			}
  			assert(idx>=0);
  			cacheAddrs[idx]=addr;
  			cacheAddrTick[idx]=curTick();
  		}
  		if(!foundInTemp && should_pf && (((addr / (setMask+1)) % temporalModMax) == temporalMod)) {
  			uint64_t oldestTick = (uint64_t)-1;
  			int idx = -1;
  			for(int x=0; x<cacheMaxAssoc;x++) {
  				if(temporalAddrTick[x]<oldestTick) {idx = x; oldestTick = temporalAddrTick[x]; }
			}  			
assert(idx>=0);
  			temporalAddrs[idx]=addr;
  			temporalAddrTick[idx]=curTick();
  		}  
  		return ret;		
  	}
  
  };
  SizeDuel sizeDuels[256];
  static SizeDuel* sizeDuelPtr;


  struct Hawkeye
    {
      int iteration;
      uint64_t set;
      uint64_t setMask; // address_map_rounded_entries/ maxElems - 1
      Addr logaddrs[64];
      Addr logpcs[64];
      int logsize[64];
      int maxElems = 8;
      
      Hawkeye(uint64_t mask, bool history) : iteration(0), set(0), setMask(mask)
        {
           reset();
        }
        
      Hawkeye() : iteration(0), set(0)
        {       }
      
      void reset() {
        iteration=0;
        for(int x=0;x<64;x++) {
        	logsize[x]=0;
        	logaddrs[x]=0;
        	logpcs[x]=0;
        }
        set = random_mt.random<uint64_t>(0,setMask-1);
      }
      
      void decrementOnLRU(Addr addr,AssociativeSet<TrainingUnitEntry>* trainer) {
      	 if((addr % setMask) != set) return;
         for(int y=iteration;y!=((iteration+1)&63);y=(y-1)&63) {
               if(addr==logaddrs[y]) {
               	    Addr pc = logpcs[y];
               	    TrainingUnitEntry *entry = trainer->findEntry(pc, false); //TODO: is secure
               	    if(entry!=nullptr) {
               	    	if(entry->hawkConfidence>=8) {
               	    		entry->hawkConfidence--;
               	    		//printf("%s evicted, pc %s, temporality %d\n",addr, pc,entry->temporal);
               	    	}
               	    	
               	    }
               	    return;
               }
         }            
      }
      
      void add(Addr addr,  Addr pc,AssociativeSet<TrainingUnitEntry>* trainer) {
        if((addr % setMask) != set) return;
        logaddrs[iteration]=addr;
        logpcs[iteration]=pc;
        logsize[iteration]=0;

        
        TrainingUnitEntry *entry = trainer->findEntry(pc, false); //TODO: is secure
        if(entry!=nullptr) {
          for(int y=(iteration-1)&63;y!=iteration;y=(y-1)&63) {
               
               if(logsize[y] == maxElems) {
                 //no match
                 //printf("%s above max elems, pc %s, temporality %d\n",addr, pc,entry->temporal-1);
                 entry->hawkConfidence--;
                 break;
               }
               if(addr==logaddrs[y]) {
                 //found a match
                 //printf("%s fits, pc %s, temporality %d\n",addr, pc,entry->temporal+1);
                   entry->hawkConfidence++;
                   for(int z=y;z!=iteration;z=(z+1)&63){
                   	logsize[z]++;
                   }
                break;
               }
            }            
        }
        iteration++;
        iteration = iteration % 64;
      }
      
    };



    
    
    Hawkeye hawksets[64];
    bool useHawkeye;

    /** Address Mapping entry, holds an address and a confidence counter */
    struct MarkovMapping : public TaggedEntry
    {
      	Addr index; //Just for maintaining HawkEye easily. Not real.
        Addr address;
        int lookupIndex; //Only one of lookupIndex/Address are real.
        bool confident;
        Cycles cycle_issued; // only for prefetched cache and only in simulation
        MarkovMapping() : index(0), address(0), confident(false), cycle_issued(0)
        {}


        void
        invalidate() override
        {
                TaggedEntry::invalidate();
                address = 0;
                index = 0;
                confident = false;
                cycle_issued=Cycles(0);
        }
    };
    

    /** Sample unit entry, tagged by data address, stores PC, timestamp, next element **/
    struct SampleEntry : public TaggedEntry
    {
    	TrainingUnitEntry* entry;
    	bool reused;
    	uint64_t local_timestamp;
    	Addr next;
    	bool confident;

    	SampleEntry() : entry(nullptr), reused(false), local_timestamp(0), next(0)
        {}

        void
        invalidate() override
        {
            TaggedEntry::invalidate();
        }

        void clear() {
                entry=nullptr;
                reused = false;
                local_timestamp=0;
                next = 0;
                confident=false;
        }
    };
    AssociativeSet<SampleEntry> historySampler;

    /** Test pf entry, tagged by data address**/
    struct SecondChanceEntry: public TaggedEntry
    {
    	Addr pc;
    	uint64_t global_timestamp;
    	bool used;
    };
    AssociativeSet<SecondChanceEntry> secondChanceUnit;

    /** History mappings table */
    AssociativeSet<MarkovMapping> markovTable;
    static AssociativeSet<MarkovMapping>* markovTablePtr;
    

    AssociativeSet<MarkovMapping> metadataReuseBuffer;
    bool lastAccessFromPFCache;

    MarkovMapping* getHistoryEntry(Addr index, bool is_secure, bool replace, bool readonly, bool clearing, bool hawk);

  public:
    Triangel(const TriangelPrefetcherParams &p);
    ~Triangel() = default;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_TRIANGEL_HH__
