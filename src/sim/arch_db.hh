
#ifndef __SIM_ARCH_DB_H__
#define __SIM_ARCH_DB_H__

#include <sqlite3.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

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
    bool dumpBopValidationTrace;
    bool dumpSMSTrainTrace;
    bool dumpStrideTrainTrace;
    bool dumpDespacitoTrainTrace;
    bool dumpL1WayPreTrace;
    bool dumpVaddrTrace;
    bool dumpLifetime;
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
                       uint64_t completed, uint64_t committed, uint64_t writenback, int pf_src);

    void l1PFTraceWrite(Tick tick, Addr trigger_pc, Addr trigger_vaddr, Addr pf_vaddr, int pf_src);

    void bopTrainTraceWrite(Tick tick, Addr old_addr, Addr cur_addr, Addr offset, int score, bool miss);
    void bopValidationTraceWrite(
      Tick tick, const char *event, const char *bop_name,
      Addr trigger_pc, Addr trigger_addr, Addr validation_addr, Addr pf_addr,
      int64_t best_offset, int best_score, int round, bool late,
      bool trigger_is_demand, bool trigger_cache_miss, int trigger_pf_source,
      bool trigger_pf_first_hit, bool trigger_pf_hit, int issue_enabled,
      int validation_enabled, int validation_hit, bool suppressed,
      bool generated, bool buffered, bool filtered, bool filter_passed,
      bool pc_confidence_enabled, int pc_index, Addr pc_tag,
      int pc_entry_hit, int pc_confidence, int pc_state,
      bool pc_sampled, int pc_epoch, int pc_low_entry_miss_streak);
    void bopValidationConfidenceUpdateTraceWrite(
      Tick tick, const char *bop_name, Addr trigger_pc, unsigned int pc_index,
      Addr pc_tag, bool validation_hit, unsigned int participants,
      int confidence_before, int confidence_after, bool decayed,
      bool offset_changed, unsigned int epoch_after,
      int low_entry_miss_streak_before, int low_entry_miss_streak_after,
      bool low_entry_hysteresis_held,
      bool low_entry_hysteresis_transition);
    void bopValidationOutcomeTraceWrite(
      Tick tick, const char *event, Addr addr, Addr pc, int pf_source,
      bool is_demand, bool cache_miss);
    void smsTrainTraceWrite(Tick tick, Addr old_addr, Addr cur_addr, Addr trigger_offset, int conf, bool miss);
    void strideTraceWrite(Tick tick, Addr addr, Addr PC, Addr hashPC, bool hit, bool isFirstShot, bool miss, bool is_train);
    void despacitoTraceWrite(Tick tick, Addr vaddr, Addr paddr, Addr PC, bool hasPC, bool miss, bool is_train);
    void dcacheWayPreTrace(Tick tick, uint64_t pc, uint64_t vaddr, int way, int is_write);
    void vaddrTrace(Tick tick, uint64_t pc, uint64_t vaddr, int hit);
    char memTraceSQLBuf[4096];
};


} // namespace gem5

#endif
