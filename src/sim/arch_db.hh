
#ifndef __SIM_ARCH_DB_H__
#define __SIM_ARCH_DB_H__

#include <sqlite3.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "base/logging.hh"
#include "base/types.hh"
#include "cpu/pred/general_arch_db.hh"
#include "params/ArchDBer.hh"
#include "sim/sim_exit.hh"
#include "sim/sim_object.hh"
#include "sim/system.hh"

namespace gem5{

class BaseCache;

class DBTraceManager
{
  std::string _name;
  std::map<std::string, DataType> _fields;
  sqlite3 *_db;
public:
  DBTraceManager(const char *name, std::vector<std::pair<std::string, DataType>> fields, sqlite3 *db) {
    _name = name;
    for (auto it = fields.begin(); it != fields.end(); it++) {
      _fields[it->first] = it->second;
    }
    _db = db;
  }
  DBTraceManager() {}
  void init_table();
  void write_record(const Record &record);
};

class ArchDBer : public SimObject
{
  public:
    PARAMS(ArchDBer);
    ArchDBer(const Params &p);

    //let db start recording
    void start_recording();

    //variables from chisel generate cpp
    bool dumpGlobal;
    bool dumpRolling;
    bool dumpMemTrace;
    bool dumpL1PfTrace;
    bool dumpL1EvictTrace;
    bool dumpL2EvictTrace;
    bool dumpL3EvictTrace;
    bool dumpL1MissTrace;
    bool dumpBopTrainTrace;
    bool dumpSMSTrainTrace;
    bool dumpStrideTrainTrace;
    bool dumpStrideOrderTrace;
    bool dumpStrideDepthCtrlTrace;
    bool dumpForceHitTrace;
    bool dumpSnoopFilterTrace;
    bool dumpTrainFilterTrace;
    bool dumpDespacitoTrainTrace;
    bool dumpL1WayPreTrace;
    bool dumpVaddrTrace;
    bool dumpLifetime;
    bool dumpLifetimeMore;

    sqlite3 *mem_db;
    sqlite3 *panic_trace_db;
    char * zErrMsg;
    int rc;
    //path to save
    std::string db_path;
    // a trace corrsponds to a table
    std::map<std::string, DBTraceManager> _traces;
    std::unordered_set<uint64_t> trackedForceHitLines;

    void create_table(const std::string &sql);
    void execmdOn(sqlite3 *db, const std::string &cmd, const char *db_name);
    bool shouldMirrorPanicTable(const std::string &sql) const;
    void mirrorPanicTraceWrite(const std::string &sql, bool force_hit,
                               bool snoop_filter);

    void save_db();
  public:
    void execmd(std::string cmd);

    DBTraceManager *addAndGetTrace(const char *name, std::vector<std::pair<std::string, DataType>> fields);

    bool get_dump_rolling() { return dumpRolling; }

    void L1MissTrace_write(
      uint64_t pc,
      uint64_t source,
      uint64_t paddr,
      uint64_t vaddr,
      uint64_t stamp,
      const char * site
    );

    void evictTraceWrite(int cache_level, Tick tick, uint64_t paddr, uint64_t stamp, const char *site);

    void memTraceWrite(Tick tick, bool is_load, Addr pc, Addr vaddr, Addr paddr, uint64_t issued, uint64_t translated,
                       uint64_t completed, uint64_t committed, uint64_t writenback, int pf_src, uint64_t seqNum);

    void l1PFTraceWrite(Tick tick, Addr trigger_pc, Addr trigger_vaddr, Addr pf_vaddr, int pf_src);

    void bopTrainTraceWrite(Tick tick, Addr old_addr, Addr cur_addr, Addr offset, int score, bool miss);
    void smsTrainTraceWrite(Tick tick, Addr old_addr, Addr cur_addr, Addr trigger_offset, int conf, bool miss);
    void strideTraceWrite(Tick tick, Addr addr, Addr PC, Addr hashPC, bool hit,
                          bool isFirstShot, bool miss, bool is_train,
                          uint64_t triggerSeqNum);
    void strideOrderTraceWrite(Tick tick, const char *stage, uint64_t seqNum,
                               Addr pc, Addr addr, Addr blockAddr,
                               bool isLoad, bool miss, int pfSource,
                               int pfDepth, Tick observedTick,
                               int queueSize, const char *reason);
    void strideDepthFeedbackTraceWrite(
        Tick tick, int level, const char *eventKind, int pfSource,
        int pfDepth, int aheadLevel, int globalL1Depth, int globalL2Gap,
        int effectiveL2Depth, uint64_t lateStrongWindow,
        uint64_t lateMSHRWindow, uint64_t lateCacheWindow,
        uint64_t timelyWindow, const char *site);
    void strideDepthDecisionTraceWrite(
        Tick tick, int level, const char *evalSite, uint64_t totalFeedback,
        uint64_t feedbackWindow, uint64_t weightedLate,
        uint64_t weightedTotal, const char *action, const char *reason,
        int oldL1Depth, int oldL2Gap, int oldEffectiveL2Depth,
        int newL1Depth, int newL2Gap, int newEffectiveL2Depth,
        uint64_t preLateStrongWindow, uint64_t preLateMSHRWindow,
        uint64_t preLateCacheWindow, uint64_t preTimelyWindow,
        uint64_t postLateStrongWindow, uint64_t postLateMSHRWindow,
        uint64_t postLateCacheWindow, uint64_t postTimelyWindow,
        uint64_t strongLateWeight, uint64_t mshrLateWeight,
        uint64_t cacheLateWeight, uint64_t raiseThresholdPct,
        uint64_t lowerWeakLatePct, uint64_t lowerTimelyPct,
        const char *site);
    void trackForceHitLine(uint64_t lineKey);
    bool isTrackedForceHitLine(uint64_t lineKey) const;
    void forceHitTraceWrite(
        Tick tick, const char *cache, const char *event, Addr lineAddr,
        Addr pc, Addr vaddr, const char *cmd, uint64_t reqPtr,
        uint64_t pktPtr, int cacheLevel, bool isSecure, bool fromCache,
        bool needsWritable, bool needsResponse, bool hasSharers,
        bool blockCached, bool mshrHit, bool wbHit, bool allocated,
        bool blkValid, bool blkReadable, bool blkWritable, bool blkDirty,
        const char *site);
    void snoopFilterTraceWrite(
        Tick tick, const char *filterName, const char *event, Addr lineAddr,
        const char *cmd, uint64_t reqPortMask, uint64_t requestedBefore,
        uint64_t holderBefore, uint64_t requestedAfter,
        uint64_t holderAfter, bool allocate, bool isHit, bool isSecure,
        bool fromCache, bool needsResponse, bool cacheResponding,
        bool blockCached, bool hasSharers, const char *reqPortName,
        const char *rspPortName, const char *site);
    void trainFilterTraceWrite(Tick tick, const char *stage, uint64_t seqNum,
                               Addr pc, Addr addr, Addr blockAddr,
                               bool isLoad, bool miss, int pfSource,
                               int pfDepth, Tick observedTick,
                               int queueSize, const char *reason);
    void loadOrderTraceWrite(Tick tick, const char *stage, uint64_t seqNum,
                             Addr pc, Addr vaddr, Addr paddr,
                             const char *reason);
    void despacitoTraceWrite(Tick tick, Addr vaddr, Addr paddr, Addr PC, bool hasPC, bool miss, bool is_train);
    void dcacheWayPreTrace(Tick tick, uint64_t pc, uint64_t vaddr, int way, int is_write);
    void vaddrTrace(Tick tick, uint64_t pc, uint64_t vaddr, int hit);
    char memTraceSQLBuf[1024];
};


} // namespace gem5

#endif
