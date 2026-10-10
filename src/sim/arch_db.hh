
#ifndef __SIM_ARCH_DB_H__
#define __SIM_ARCH_DB_H__

#include <sqlite3.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "base/logging.hh"
#include "base/types.hh"
#include "cpu/pred/general_arch_db.hh"
#include "params/ArchDBer.hh"
#include "sim/sim_exit.hh"
#include "sim/sim_object.hh"
#include "sim/system.hh"

namespace gem5{

class BaseCache;

struct CacheTraceRecord
{
    std::string cache, event, detail;
    Tick tick = 0;
    uint64_t mshr = 0, parent = 0;
    uint64_t request = 0, target = 0, relatedRequest = 0;
    Addr blockAddr = 0;
    bool secure = false;
    int requestor = -1, context = -1;
    InstSeqNum seq = 0;
    int targets = 0, allocated = 0, heldCredits = 0, blockedMask = 0;
};

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
    bool dumpDespacitoTrainTrace;
    bool dumpL1WayPreTrace;
    bool dumpVaddrTrace;
    bool dumpLifetime;
    bool dumpCausal;
    bool dumpLifetimeMore;

    sqlite3 *mem_db;
    char * zErrMsg;
    int rc;
    //path to save
    std::string db_path;
    // a trace corrsponds to a table
    std::map<std::string, DBTraceManager> _traces;

    void create_table(const std::string &sql);

    void save_db();
    // Flush compressed trace records before the in-memory DB is saved.
    std::vector<std::function<void()>> traceFinalizers;
  public:
    void execmd(std::string cmd);
    uint64_t cacheTraceWrite(const CacheTraceRecord &record);

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
                       uint64_t completed, uint64_t committed, uint64_t writenback, int pf_src);

    void l1PFTraceWrite(Tick tick, Addr trigger_pc, Addr trigger_vaddr, Addr pf_vaddr, int pf_src);

    void bopTrainTraceWrite(Tick tick, Addr old_addr, Addr cur_addr, Addr offset, int score, bool miss);
    void smsTrainTraceWrite(Tick tick, Addr old_addr, Addr cur_addr, Addr trigger_offset, int conf, bool miss);
    void strideTraceWrite(Tick tick, Addr addr, Addr PC, Addr hashPC, bool hit, bool isFirstShot, bool miss, bool is_train);
    void despacitoTraceWrite(Tick tick, Addr vaddr, Addr paddr, Addr PC, bool hasPC, bool miss, bool is_train);
    void dcacheWayPreTrace(Tick tick, uint64_t pc, uint64_t vaddr, int way, int is_write);
    void vaddrTrace(Tick tick, uint64_t pc, uint64_t vaddr, int hit);
    char memTraceSQLBuf[1024];
};


} // namespace gem5

#endif
