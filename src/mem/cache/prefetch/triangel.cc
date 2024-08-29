
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

#include "debug/Triangel.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/TriangelPrefetcher.hh"
#include <cmath>


namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

int Triangel::target_size=0;
int Triangel::current_size=0;
int64_t Triangel::global_timestamp=0;
AssociativeSet<Triangel::MarkovMapping>* Triangel::markovTablePtr=NULL;
std::vector<uint32_t> Triangel::setPrefetch(17,0);
Triangel::SizeDuel* Triangel::sizeDuelPtr=nullptr;
bloom* Triangel::blptr = nullptr;

Triangel::Triangel(
    const TriangelPrefetcherParams &p)
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
    sctags(p.sctags),
    max_size(p.address_map_actual_entries),
    size_increment(p.address_map_actual_entries/p.address_map_max_ways),
    second_chance_timestamp(0),
    //global_timestamp(0),
    //current_size(0),
    //target_size(0),
    maxWays(p.address_map_max_ways),    
    bl(),
    bloomset(-1),
    way_idx(p.address_map_actual_entries/(p.address_map_max_ways*p.address_map_actual_cache_assoc),0),
    globalReuseConfidence(7,64),
    globalPatternConfidence(7,64),
    globalHighPatternConfidence(7,64),
    trainingUnit(p.training_unit_assoc, p.training_unit_entries,
                 p.training_unit_indexing_policy,
                 p.training_unit_replacement_policy),
    lookupAssoc(p.lookup_assoc),
    lookupOffset(p.lookup_offset),        
    //setPrefetch(cachetags->getWayAllocationMax()+1,0),      
    useHawkeye(p.use_hawkeye),
    historySampler(p.sample_assoc,
    		  p.sample_entries,
    		  p.sample_indexing_policy,
    		  p.sample_replacement_policy),
    secondChanceUnit(p.secondchance_assoc,
    		  p.secondchance_entries,
    		  p.secondchance_indexing_policy,
    		  p.secondchance_replacement_policy),
    markovTable(p.address_map_rounded_cache_assoc,
                          p.address_map_rounded_entries,
                          p.address_map_cache_indexing_policy,
                          p.address_map_cache_replacement_policy,
                          MarkovMapping()),                   
    metadataReuseBuffer(p.metadata_reuse_assoc,
                          p.metadata_reuse_entries,
                          p.metadata_reuse_indexing_policy,
                          p.metadata_reuse_replacement_policy,
                          MarkovMapping()),
    lastAccessFromPFCache(false)
{	
	markovTablePtr = &markovTable;

	setPrefetch.resize(cachetags->getWayAllocationMax()+1,0);
	assert(p.address_map_rounded_entries / p.address_map_rounded_cache_assoc == p.address_map_actual_entries / p.address_map_actual_cache_assoc);
	markovTable.setWayAllocationMax(p.address_map_actual_cache_assoc);
	assert(cachetags->getWayAllocationMax()> maxWays);
	int bloom_size = p.address_map_actual_entries/128 < 1024? 1024: p.address_map_actual_entries/128;
	assert(bloom_init2(&bl,bloom_size, 0.01)==0);
	blptr = &bl;
	for(int x=0;x<64;x++) {
		hawksets[x].setMask = p.address_map_actual_entries/ hawksets[x].maxElems;
		hawksets[x].reset();
	}
		sizeDuelPtr= sizeDuels;	
	for(int x=0;x<64;x++) {
		sizeDuelPtr[x].reset(size_increment/p.address_map_actual_cache_assoc - 1 ,p.address_map_actual_cache_assoc,cachetags->getWayAllocationMax());
	}

	for(int x=0;x<1024;x++) {
		lookupTable[x]=0;
    		lookupTick[x]=0;
	}	
	current_size = 0;
	target_size=0;

}


bool
Triangel::randomChance(int reuseConf, int replaceRate) {
	replaceRate -=8;

	uint64_t baseChance = 1000000000l * historySampler.numEntries / markovTable.numEntries;
	baseChance = replaceRate>0? (baseChance << replaceRate) : (baseChance >> (-replaceRate));
	baseChance = reuseConf < 3 ? baseChance / 16 : baseChance;
	uint64_t chance = random_mt.random<uint64_t>(0,1000000000ul);

	return baseChance >= chance;
}

void
Triangel::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses)
{

    Addr addr = blockIndex(pfi.getAddr());
    second_chance_timestamp++;
    
    // This prefetcher requires a PC
    if (!pfi.hasPC() || pfi.isWrite()) {
if(!use_bloom) {
	    for(int x=0;x<(smallduel? 32 :64);x++) {
		int res =    	sizeDuelPtr[x].checkAndInsert(addr,false);
		if(res==0)continue;
		int cache_hit = res%128;
		int cache_set = cache_hit-1;
		assert(!cache_hit || (cache_set<setPrefetch.size()-1 && cache_set>=0));
		if(cache_hit) for(int y= setPrefetch.size()-2-cache_set; y>=0; y--) setPrefetch[y]++; 
		// cache partition hit at this size or bigger. So hit in way 14 = y=17-2-14=1 and 0: would hit with 0 ways reserved or 1, not 2.

	    }
}
        return;
    }

    bool is_secure = pfi.isSecure();
    Addr pc = pfi.getPC()>>2; //Shifted by 2 to help Arm indexing. Bit fake; really should xor in these bits with upper bits.


    // Looks up the last address at this PC
    TrainingUnitEntry *entry = trainingUnit.findEntry(pc, is_secure);
    bool correlated_addr_found = false;
    Addr index = 0;
    Addr target = 0;
    
    const int upperHistory=globalPatternConfidence>64?7:8;
    const int highUpperHistory=globalHighPatternConfidence>64?7:8;
    const int superHistory=14;
    
    const int upperReuse=globalReuseConfidence>64?7:8;
    
    //const int globalThreshold = 9;
    
    bool should_pf=false;
    bool should_hawk=false;
    bool should_sample = false;
    if (entry != nullptr) { //This accesses the training table at this PC.
        trainingUnit.accessEntry(entry);
        correlated_addr_found = true;
        index = entry->lastAddress;

        if(addr == entry->lastAddress) return; // to avoid repeat trainings on sequence.
	if(entry->highPatternConfidence >= superHistory || !use_pattern2) entry->currently_twodist_pf=true;
	if(entry->patternConfidence < upperHistory && use_pattern2) entry->currently_twodist_pf=false; 
        //if very sure, index should be lastLastAddress. TODO: We could also try to learn timeliness here, by tracking PCs at the MSHRs.
        if(entry->currently_twodist_pf && should_lookahead) index = entry->lastLastAddress;
        target = addr;
        should_pf = (entry->reuseConfidence > upperReuse || !use_reuse) && (entry->patternConfidence > upperHistory || !use_pattern); //8 is the reset point.

        global_timestamp++;

    }

    if(entry==nullptr && (randomChance(8,8) || ((!use_reuse || globalReuseConfidence > 64) && (!use_pattern2 || globalHighPatternConfidence > 64) && (!use_pattern || globalPatternConfidence > 64)))){ //only start adding samples for frequent entries.
	//TODO: should this instead just be tagless?
    	if(!((!use_reuse || globalReuseConfidence > 64) && (!use_pattern2 || globalHighPatternConfidence > 64)  && (!use_pattern || globalPatternConfidence > 64)))should_sample = true;
        entry = trainingUnit.findVictim(pc);
        DPRINTF(Triangel, "Replacing Training Entry %x\n",pc);
        assert(entry != nullptr);
        assert(!entry->isValid());
        trainingUnit.insertEntry(pc, is_secure, entry);
        //printf("local timestamp %ld\n", entry->local_timestamp);
        if(globalHighPatternConfidence>96) entry->currently_twodist_pf=true;
    }



    if(correlated_addr_found) {
    	//Check second-chance sampler for a recent history. If so, update pattern confidence accordingly.
    	SecondChanceEntry* tentry = secondChanceUnit.findEntry(addr, is_secure);
    	if(tentry!= nullptr && !tentry->used) {
    		tentry->used=true;
    		TrainingUnitEntry *pentry = trainingUnit.findEntry(tentry->pc, is_secure);
    		 if((!timed_scs || (tentry->global_timestamp + 512 > second_chance_timestamp)) && pentry != nullptr) {  
  			if(tentry->pc==pc) {
	  			pentry->patternConfidence++;
	    			pentry->highPatternConfidence++;
	    			globalPatternConfidence++;
	    			globalHighPatternConfidence++;
    			}
		} else if(pentry!=nullptr) {
				    			pentry->patternConfidence--;
			    			globalPatternConfidence--;
			    			if(!perfbias) { pentry->patternConfidence--; globalPatternConfidence--;} //bias 
			    			for(int x=0;x<(perfbias? 2 : 5);x++) {pentry->highPatternConfidence--;  globalHighPatternConfidence--;}
		}
    	}
    	
    	//Check history sampler for entry.
    	SampleEntry *sentry = historySampler.findEntry(entry->lastAddress, is_secure);
    	if(sentry != nullptr && sentry->entry == entry) {    		
    		
		int64_t distance = sentry->entry->local_timestamp - sentry->local_timestamp;
		if(distance > 0 && distance < max_size) { entry->reuseConfidence++;globalReuseConfidence++;}
		else if(!sentry->reused){ entry->reuseConfidence--;globalReuseConfidence--;}
    		sentry->reused = true;

     	    	DPRINTF(Triangel, "Found reuse for addr %x, PC %x, distance %ld (train %ld vs sample %ld) confidence %d\n",addr, pc, distance, entry->local_timestamp, sentry->local_timestamp, entry->reuseConfidence+0);
     	    	
		bool willBeConfident = addr == sentry->next;
    		
    		if(addr == sentry->next ||  (use_scs && sctags->findBlock(sentry->next<<lBlkSize,is_secure) && !sctags->findBlock(sentry->next<<lBlkSize,is_secure)->wasPrefetched())) {
    			if(addr == sentry->next) {
    				entry->patternConfidence++;
    				entry->highPatternConfidence++;
    				globalPatternConfidence++;
	    			globalHighPatternConfidence++;
    			}
    			//if(entry->replaceRate < 8) entry->replaceRate.reset();
    		}
    		
    		else {
    			//We haven't spotted the (x,y) pattern we expect, on seeing y. So put x in the SCS.
			if(use_scs) {
				SecondChanceEntry* tentry = secondChanceUnit.findVictim(addr);
				if(tentry->pc !=0 && !tentry->used) {
    			   		TrainingUnitEntry *pentry = trainingUnit.findEntry(tentry->pc, is_secure);	
    			   		if(pentry != nullptr) {
			    			pentry->patternConfidence--;
			    			globalPatternConfidence--;
			    			if(!perfbias) { pentry->patternConfidence--; globalPatternConfidence--;} //bias 
			    			for(int x=0;x<(perfbias? 2 : 5);x++) {pentry->highPatternConfidence--;  globalHighPatternConfidence--;  	 }		   		
    			   		}			
				}
	    			secondChanceUnit.insertEntry(sentry->next, is_secure, tentry);
	    			tentry->pc = pc;
	    			tentry->global_timestamp = second_chance_timestamp;
	    			tentry->used = false;
    			} else {
    				entry->patternConfidence--;
    				globalPatternConfidence--;
			    	if(!perfbias) { entry->patternConfidence--; globalPatternConfidence--;} 
			    	for(int x=0;x<(perfbias? 2 : 5);x++) {entry->highPatternConfidence--;  globalHighPatternConfidence--;  	 }	 			   		
    			}
    		}
    		
        	if(addr == sentry->next) DPRINTF(Triangel, "Match for address %x confidence %d\n",addr, entry->patternConfidence+0);
    		     	    	if(sentry->entry == entry) if(!useSampleConfidence || !sentry->confident) sentry->next=addr;
    		     	    	sentry->confident = willBeConfident;
    	}
    	else if(should_sample || randomChance(entry->reuseConfidence,entry->replaceRate)) {
    		//Fill sample table, as we're taking a sample at this PC. Should_sample also set by randomChance earlier, on first insert of PC intro training table.
    		sentry = historySampler.findVictim(entry->lastAddress);
    		assert(sentry != nullptr);
    		if(sentry->entry !=nullptr) {
    			    TrainingUnitEntry *pentry = sentry->entry;
    			    if(pentry != nullptr) {
    			    	    int64_t distance = pentry->local_timestamp - sentry->local_timestamp;
    			    	    DPRINTF(Triangel, "Replacing Entry %x with PC %x, old distance %d\n",sentry->entry, pc, distance);
    			    	    if(distance > max_size) {
    			    	        //TODO: Change max size to be relative, based on current tracking set?
    			            	trainingUnit.accessEntry(pentry);
    			            	if(!sentry->reused) { 
    			            	    	//Reuse conf decremented, as very old.
    			            		pentry->reuseConfidence--;
						globalReuseConfidence--;
    			            	}
    			            	entry->replaceRate++; //only replacing oldies -- can afford to be more aggressive.
    			            } else if(distance > 0 && !sentry->reused) { //distance goes -ve due to lack of training-entry space
    			            	entry->replaceRate--;
    			            }
    			    } else entry->replaceRate++;
    		}
    		assert(!sentry->isValid());
    		sentry->clear();
    		historySampler.insertEntry(entry->lastAddress, is_secure, sentry);
    		sentry->entry = entry;
    		sentry->reused = false;
    		sentry->local_timestamp = entry->local_timestamp+1;
    		sentry->next = addr;
    		sentry->confident=false;
    	}
    }
    
    if(!use_bloom) {
	    
	    for(int x=0;x<(smallduel? 32 :64);x++) {
	    	//Here we update the size duellers, to work out for each cache set whether it is better to be markov table or L3 cache.
		int res =    	sizeDuelPtr[x].checkAndInsert(addr,should_pf); //TODO: combine with hawk?
		if(res==0)continue;
		const int ratioNumer=(perfbias?4:2);
		const int ratioDenom=4;//should_pf && entry->highPatternConfidence >=upperHistory? 4 : 8;
		int cache_hit = res%128; //This is just bit encoding of cache hits.
		int pref_hit = res/128; //This is just bit encoding of prefetch hits.
		int cache_set = cache_hit-1; //Encodes which nth most used replacement-state we hit at, if any.
		int pref_set = pref_hit-1; //Encodes which nth most used replacement-state we hit at, if any.
		assert(!cache_hit || (cache_set<setPrefetch.size()-1 && cache_set>=0));
		assert(!pref_hit || (pref_set<setPrefetch.size()-1 && pref_set>=0));
		if(cache_hit) for(int y= setPrefetch.size()-2-cache_set; y>=0; y--) setPrefetch[y]++; 
		// cache partition hit at this size or bigger. So hit in way 14 = y=17-2-14=1 and 0: would hit with 0 ways reserved or 1, not 2.
		if(pref_hit)for(int y=pref_set+1;y<setPrefetch.size();y++) setPrefetch[y]+=(ratioNumer*sizeDuelPtr[x].temporalModMax)/ratioDenom; 
		// ^ pf hit at this size or bigger. one-indexed (since 0 is an alloc on 0 ways). So hit in way 0 = y=1--16 ways reserved, not 0.
		
		//if(cache_hit) printf("Cache hit\n");
		//else printf("Prefetch hit\n");
	    }
	    

	    if(global_timestamp > 500000) {
	    //Here we choose the size of the Markov table based on the optimum for the last epoch
	    int counterSizeSeen = 0;

	    for(int x=0;x<setPrefetch.size() && x*size_increment <= max_size;x++) {
	    	if(setPrefetch[x]>counterSizeSeen) {
	    		 target_size= size_increment*x;
	    		 counterSizeSeen = setPrefetch[x];
	    	}
	    }

	    int currentscore = setPrefetch[current_size/size_increment];
	    currentscore = currentscore + (currentscore>>4); //Slight bias against changing for minimal benefit.
	    int targetscore = setPrefetch[target_size/size_increment];

	    if(target_size != current_size && targetscore>currentscore) {
	    	current_size = target_size;
		printf("size: %d, tick %ld \n",current_size,curTick());
		//for(int x=0;x<setPrefetch.size(); x++) {
		//	printf("%d: %d\n", x, setPrefetch[x]);
		//}
		assert(current_size >= 0);
		
	
		for(int x=0;x<64;x++) {
			hawksets[x].setMask = current_size / hawksets[x].maxElems;
			hawksets[x].reset();
		}
		std::vector<MarkovMapping> ams;
			     	if(should_rearrange) {		    	
					for(MarkovMapping am: *markovTablePtr) {
					    		if(am.isValid()) ams.push_back(am);
					}
					for(MarkovMapping& am: *markovTablePtr) {
				    		am.invalidate(); //for RRIP's sake
				    	}
			    	}
			    	TriangelHashedSetAssociative* thsa = dynamic_cast<TriangelHashedSetAssociative*>(markovTablePtr->indexingPolicy);
		  				if(thsa) { thsa->ways = current_size/size_increment; thsa->max_ways = maxWays; assert(thsa->ways <= thsa->max_ways);}
		  				else assert(0);
			    	//rearrange conditionally
				if(should_rearrange) {        	
				    	if(current_size >0) {
					      	for(MarkovMapping am: ams) {
					    		   MarkovMapping *mapping = getHistoryEntry(am.index, am.isSecure(),true,false,true,true);
					    		   mapping->address = am.address;
					    		   mapping->index=am.index;
					    		   mapping->confident = am.confident;
					    		   mapping->lookupIndex=am.lookupIndex;
					    		   markovTablePtr->weightedAccessEntry(mapping,1,false); //For RRIP, touch
					    	}    	
				    	}
			    	}
			    	
			    	for(MarkovMapping& am: *markovTablePtr) {
				    if(thsa->ways==0 || (thsa->extractSet(am.index) % maxWays)>=thsa->ways)  am.invalidate();
				}
			    	cachetags->setWayAllocationMax(setPrefetch.size()-1-thsa->ways);  	
	    } 
			printf("End of epoch:\n");
		for(int x=0;x<setPrefetch.size(); x++) {

			printf("%d: %d\n", x, setPrefetch[x]);
		}
	    	global_timestamp=0;
		for(int x=0;x<setPrefetch.size();x++) {
		    	setPrefetch[x]=0;
		}
	    	//Reset after 2 million prefetch accesses -- not quite the same as after 30 million insts but close enough
	     }
	     
     }


    if(useHawkeye && correlated_addr_found && should_pf) {
        // If a correlation was found, update the Markov table accordingly
        	for(int x=0; x<64; x++)hawksets[x].add(addr,pc,&trainingUnit);
        	should_hawk = entry->hawkConfidence>7;
    }
    
    if(use_bloom) {
	    if(correlated_addr_found && should_pf) {
	    	if(bloomset==-1) bloomset = index&127;
	    	if((index&127)==bloomset) {
			int add = bloom_add(blptr, &index, sizeof(Addr));
			if(!add) { target_size+=192;
			
			//printf("Bloom: pc %ld conf %d %d %d rate %d\n", pc, entry->reuseConfidence+0,entry->patternConfidence+0,entry->highPatternConfidence+0,entry->replaceRate+0);
			}
		}

	    }
	    
	    while(target_size > current_size
		    		     && target_size > size_increment / 8 && current_size < max_size) {
		    		        //check for size_increment to leave empty if unlikely to be useful.
		    			current_size += size_increment;
		    			printf("size: %d, tick %ld \n",current_size,curTick());
		    			assert(current_size <= max_size);
		    			assert(cachetags->getWayAllocationMax()>1);
		    			cachetags->setWayAllocationMax(cachetags->getWayAllocationMax()-1);

		    	std::vector<MarkovMapping> ams;
		        if(should_rearrange) {        	
			     	for(MarkovMapping am: *markovTablePtr) {
			    		if(am.isValid()) ams.push_back(am);
			    	}
				for(MarkovMapping& am: *markovTablePtr) {
			    		am.invalidate(); //for RRIP's sake
			    	}
		    	}
		    	TriangelHashedSetAssociative* thsa = dynamic_cast<TriangelHashedSetAssociative*>(markovTablePtr->indexingPolicy);
	  		if(thsa) { thsa->ways++; thsa->max_ways = maxWays; assert(thsa->ways <= thsa->max_ways);}
	  		else assert(0);
		    	//TODO: rearrange conditionally
		        if(should_rearrange) {        	            	
				for(MarkovMapping am: ams) {
				    		   MarkovMapping *mapping = getHistoryEntry(am.index, am.isSecure(),true,false, true, true);
				    		   mapping->address = am.address;
				    		   mapping->index=am.index;
				    		   mapping->confident = am.confident;
				    		   mapping->lookupIndex=am.lookupIndex;
				    		   markovTablePtr->weightedAccessEntry(mapping,1,false); //For RRIP, touch
				} 
			}  
		    			//increase associativity of the set structure by 1!
		    			//Also, decrease LLC cache associativity by 1.
	    }

		if(global_timestamp > 2000000) {
	    	//Reset after 2 million prefetch accesses -- not quite the same as after 30 million insts but close enough

	    	while((target_size <= current_size - size_increment  || target_size < size_increment / 8)  && current_size >=size_increment) {
	    		//reduce the assoc by 1.
	    		//Also, increase LLC cache associativity by 1.
	    		current_size -= size_increment;
	    		printf("size: %d, tick %ld \n",current_size,curTick());
		    	assert(current_size >= 0);
		    	std::vector<MarkovMapping> ams;
		     	if(should_rearrange) {		    	
				for(MarkovMapping am: *markovTablePtr) {
				    		if(am.isValid()) ams.push_back(am);
				}
				for(MarkovMapping& am: *markovTablePtr) {
			    		am.invalidate(); //for RRIP's sake
			    	}
		    	}
		    	TriangelHashedSetAssociative* thsa = dynamic_cast<TriangelHashedSetAssociative*>(markovTablePtr->indexingPolicy);
	  				if(thsa) { assert(thsa->ways >0); thsa->ways--; }
	  				else assert(0);
		    	//rearrange conditionally
		        if(should_rearrange) {        	
			    	if(current_size >0) {
				      	for(MarkovMapping am: ams) {
				    		   MarkovMapping *mapping = getHistoryEntry(am.index, am.isSecure(),true,false,true,true);
				    		   mapping->address = am.address;
				    		   mapping->index=am.index;
				    		   mapping->confident = am.confident;
				    		   mapping->lookupIndex=am.lookupIndex;
				    		   markovTablePtr->weightedAccessEntry(mapping,1,false); //For RRIP, touch
				    	}    	
			    	}
		    	}
		    	
		    	for(MarkovMapping& am: *markovTablePtr) {
			    if(thsa->ways==0 || (thsa->extractSet(am.index) % maxWays)>=thsa->ways)  am.invalidate();
			}
		    	
		    	
		    	

	    		cachetags->setWayAllocationMax(cachetags->getWayAllocationMax()+1);
	    	}
	    	target_size = 0;
	    	global_timestamp=0;
	    	bloom_reset(blptr);
	    	bloomset=-1;
	    }
    }
    
    
    if (correlated_addr_found && should_pf && (current_size>0)) {
        // If a correlation was found, update the Markov table accordingly
	//DPRINTF(Triangel, "Tabling correlation %x to %x, PC %x\n", index << lBlkSize, target << lBlkSize, pc);
	MarkovMapping *mapping = getHistoryEntry(index, is_secure,false,false,false, should_hawk);
	if(mapping == nullptr) {
        	mapping = getHistoryEntry(index, is_secure,true,false,false, should_hawk);
        	mapping->address = target;
        	mapping->index=index; //for HawkEye
        	mapping->confident = false;
        }
        assert(mapping != nullptr);
        bool confident = mapping->address == target; 
        bool wasConfident = mapping->confident;
        mapping->confident = confident; //Confidence is just used for replacement. I haven't tested how important it is for performance to use it; this is inherited from Triage.
        if(!wasConfident) {
        	mapping->address = target;
        }
        if(wasConfident && confident && use_mrb) {
        	MarkovMapping *cached_entry =
        		metadataReuseBuffer.findEntry(index, is_secure);
        	if(cached_entry != nullptr) {
        		prefetchStats.metadataAccesses--;
        		//No need to access L3 again, as no updates to be done.
        	}
        }
        
        int index=0;
        uint64_t time = -1;
        if(lookupAssoc>0){
		int lookupMask = (1024/lookupAssoc)-1;
		int set = (target>>lookupOffset)&lookupMask;
		for(int x=lookupAssoc*set;x<lookupAssoc*(set+1);x++) {
			if(target>>lookupOffset == lookupTable[x]) {
				index=x;
				break;
			}
			if(time > lookupTick[x]) {
				time = lookupTick[x];
				index=x;
			}
		}
		
		lookupTable[index]=target>>lookupOffset;
		lookupTick[index]=curTick();
		mapping->lookupIndex=index;
        }
        
    }

    if(target != 0 && should_pf && (current_size>0)) {
  	 MarkovMapping *pf_target = getHistoryEntry(target, is_secure,false,true,false, should_hawk);
   	 unsigned deg = 0;
  	 unsigned delay = cacheDelay;
  	 bool high_degree_pf = pf_target != nullptr
  	         && (entry->highPatternConfidence>highUpperHistory || !use_pattern2)/*&& pf_target->confident*/;
   	 unsigned max = high_degree_pf? degree : (should_pf? 1 : 0);
   	 //if(pf_target == nullptr && should_pf) DPRINTF(Triangel, "Target not found for %x, PC %x\n", target << lBlkSize, pc);
   	 while (pf_target != nullptr && deg < max  /*&& (pf_target->confident || entry->highPatternConfidence>upperHistory)*/
   	 ) { //TODO: do we always pf at distance 1 if not confident?
    		DPRINTF(Triangel, "Prefetching %x on miss at %x, PC \n", pf_target->address << lBlkSize, addr << lBlkSize, pc);
    		int extraDelay = cacheDelay;
    		if(lastAccessFromPFCache && use_mrb) {
    			Cycles time = curCycle() - pf_target->cycle_issued;
    			if(time >= cacheDelay) extraDelay = 0;
    			else if (time < cacheDelay) extraDelay = time;
    		}
    		
    		Addr lookup = pf_target->address;
   	        if(lookupAssoc>0){
	   	 	int index=pf_target->lookupIndex;
	   	 	int lookupMask = (1<<lookupOffset)-1;
	   	 	lookup = (lookupTable[index]<<lookupOffset) + ((pf_target->address)&lookupMask);
	   	 	lookupTick[index]=curTick();
	   	 	if(lookup == pf_target->address)prefetchStats.lookupCorrect++;
	    		else prefetchStats.lookupWrong++;
    		}
    		
    		if(extraDelay == cacheDelay) addresses.push_back(AddrPriority(lookup << lBlkSize, delay, PrefetchSourceType::Triangel));
    		delay += extraDelay;
    		deg++;
    		
    		if(deg<max /*&& pf_target->confident*/) pf_target = getHistoryEntry(lookup, is_secure,false,true,false, should_hawk);
    		else pf_target = nullptr;

   	 }
    }

        // Update the entry
    if(entry != nullptr) {
    	entry->lastLastAddress = entry->lastAddress;
    	entry->lastLastAddressSecure = entry->lastAddressSecure;
    	entry->lastAddress = addr;
    	entry->lastAddressSecure = is_secure;
    	entry->local_timestamp ++;
    }


}

Triangel::MarkovMapping*
Triangel::getHistoryEntry(Addr paddr, bool is_secure, bool add, bool readonly, bool clearing, bool hawk)
{
	//The weird parameters above control whether we replace entries, and how the number of metadata accesses are updated, for instance. They're basically a simulation thing.
  	    TriangelHashedSetAssociative* thsa = dynamic_cast<TriangelHashedSetAssociative*>(markovTablePtr->indexingPolicy);
	  				if(!thsa)  assert(0);  

    	cachetags->clearSetWay(thsa->extractSet(paddr)/maxWays, thsa->extractSet(paddr)%maxWays); 


    if(should_rearrange) {    

	    int index= paddr % (way_idx.size()); //Not quite the same indexing strategy, but close enough.
	    
	    if(way_idx[index] != thsa->ways) {
	    	if(way_idx[index] !=0) prefetchStats.metadataAccesses+= thsa->ways + way_idx[index];
	    	way_idx[index]=thsa->ways;
	    }
    }

    if(readonly) { //check the cache first.
        MarkovMapping *pf_entry =
        	use_mrb ? metadataReuseBuffer.findEntry(paddr, is_secure) : nullptr;
        if (pf_entry != nullptr) {
        	lastAccessFromPFCache = true;
        	return pf_entry;
        }
        lastAccessFromPFCache = false;
    }

    MarkovMapping *ps_entry =
        markovTablePtr->findEntry(paddr, is_secure);
    if(readonly || !add) prefetchStats.metadataAccesses++;
    if (ps_entry != nullptr) {
        // A PS-AMC line already exists
        markovTablePtr->weightedAccessEntry(ps_entry,hawk?1:0,false);
    } else {
        if(!add) return nullptr;
        ps_entry = markovTablePtr->findVictim(paddr);
        assert(ps_entry != nullptr);
        if(useHawkeye && !clearing) for(int x=0;x<64;x++) hawksets[x].decrementOnLRU(ps_entry->index,&trainingUnit);
	assert(!ps_entry->isValid());
        markovTablePtr->insertEntry(paddr, is_secure, ps_entry);
        markovTablePtr->weightedAccessEntry(ps_entry,hawk?1:0,true);
    }

    if(readonly && use_mrb) {
    	    MarkovMapping *pf_entry = metadataReuseBuffer.findVictim(paddr);
    	    metadataReuseBuffer.insertEntry(paddr, is_secure, pf_entry);
    	    pf_entry->address = ps_entry->address;
    	    pf_entry->confident = ps_entry->confident;
    	    pf_entry->cycle_issued = curCycle();
    	    //This adds access time, to set delay appropriately.
    }

    return ps_entry;
}




uint32_t
TriangelHashedSetAssociative::extractSet(const Addr addr) const
{
	//Input is already blockIndex so no need to remove block again.
    Addr offset = addr;
    
   /* const Addr hash1 = offset & ((1<<16)-1);
    const Addr hash2 = (offset >> 16) & ((1<<16)-1);
        const Addr hash3 = (offset >> 32) & ((1<<16)-1);
    */
        offset = ((offset) * max_ways) + (extractTag(addr) % ways);
        return offset & setMask;   //setMask is numSets-1

}


Addr
TriangelHashedSetAssociative::extractTag(const Addr addr) const
{
    //Input is already blockIndex so no need to remove block again.

    //Description in Triage-ISR confuses whether the index is just the 16 least significant bits,
    //or the weird index above. The tag can't be the remaining bits if we use the literal representation!


    Addr offset = addr / (numSets/max_ways); 
    int result = 0;
    
    //This is a tag# as described in the Triangel paper.
    const int shiftwidth=10;

    for(int x=0; x<64; x+=shiftwidth) {
       result ^= (offset & ((1<<shiftwidth)-1));
       offset = offset >> shiftwidth;
    }
    return result;
}



} // namespace prefetch
} // namespace gem5
