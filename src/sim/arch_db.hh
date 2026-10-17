
#ifndef __SIM_ARCH_DB_H__
#define __SIM_ARCH_DB_H__

#include <sqlite3.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

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
    bool dumpBopReplayTrace;
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
    uint64_t bopReplayPhaseId;
    uint64_t bopReplayAccessSeq;
    uint64_t bopReplayEventSeq;

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
    void bopReplayMetaTraceWrite(
      const char *bop_name, unsigned int block_size, unsigned int score_max,
      unsigned int round_max, unsigned int bad_score,
      unsigned int rr_entries, unsigned int tag_bits,
      bool delay_queue_enabled, unsigned int delay_queue_size,
      Tick delay_ticks, bool cross_page, bool adapt_offset,
      bool negative_offsets_enabled, bool auto_learning,
      unsigned int victim_offsets_list_size, unsigned int restore_cycle,
      Tick clock_period_ticks, bool student_cover_enabled,
      unsigned int student_pool_size, double student_conf_alpha,
      double student_cov_threshold, unsigned int student_teacher_top_n,
      unsigned int student_filter_entries, const std::string &student_hash_mode,
      unsigned int student_hash_count, bool student_large_offset_priority,
      double student_large_offset_priority_coeff,
      bool student_delay_queue_enabled, unsigned int student_delay_queue_size,
      Tick student_delay_ticks, const std::string &offsets);
    uint64_t bopReplayDemandTraceWrite(
      Tick tick, Addr addr, Addr pc, bool has_pc, bool cache_miss,
      int prefetch_source, bool pf_first_hit, bool pf_hit);
    void bopReplayPhaseTraceWrite(
      uint64_t phase_id, const char *phase_name, Tick start_tick);
    void bopReplayEventTraceWrite(
      uint64_t access_seq, Tick tick, const char *bop_name,
      const char *bop_kind, Addr trigger_addr, Addr trigger_pc,
      bool trigger_has_pc, bool late, int64_t teacher_offset,
      bool teacher_issue_enabled, bool student_issue_enabled,
      bool student_selected_valid, bool student_selected_enable,
      int64_t student_selected_offset, int64_t selected_offset,
      bool issue_enabled, bool cqf_allowed);
    void bopReplayDelayActionTraceWrite(
      const char *bop_name, const char *action, Tick tick, Addr addr,
      Tick process_tick, unsigned int queue_size_after);
    void smsTrainTraceWrite(Tick tick, Addr old_addr, Addr cur_addr, Addr trigger_offset, int conf, bool miss);
    void strideTraceWrite(Tick tick, Addr addr, Addr PC, Addr hashPC, bool hit, bool isFirstShot, bool miss, bool is_train);
    void despacitoTraceWrite(Tick tick, Addr vaddr, Addr paddr, Addr PC, bool hasPC, bool miss, bool is_train);
    void dcacheWayPreTrace(Tick tick, uint64_t pc, uint64_t vaddr, int way, int is_write);
    void vaddrTrace(Tick tick, uint64_t pc, uint64_t vaddr, int hit);
    char memTraceSQLBuf[1024];
};


} // namespace gem5

#endif
