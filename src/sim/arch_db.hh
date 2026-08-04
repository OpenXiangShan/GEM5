
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
    bool dumpBopReplayTrace;
    bool dumpSMSTrainTrace;
    bool dumpStrideTrainTrace;
    bool dumpDespacitoTrainTrace;
    bool dumpL1WayPreTrace;
    bool dumpVaddrTrace;
    bool dumpLifetime;
    bool dumpLifetimeMore;

    sqlite3 *mem_db;
    sqlite3_stmt *bopReplayMetaStmt;
    sqlite3_stmt *bopReplayPhaseStmt;
    sqlite3_stmt *bopReplayDemandStmt;
    sqlite3_stmt *bopReplayEventStmt;
    sqlite3_stmt *bopReplayDelayActionStmt;
    uint64_t bopReplayPhaseId;
    char * zErrMsg;
    int rc;
    //path to save
    std::string db_path;
    // a trace corrsponds to a table
    std::map<std::string, DBTraceManager> _traces;

    void create_table(const std::string &sql);

    void save_db();
  public:
    ~ArchDBer() override;
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
    void bopReplayMetaTraceWrite(
      const char *bop_name, unsigned int block_size, unsigned int score_max,
      unsigned int round_max, unsigned int bad_score, unsigned int rr_entries,
      unsigned int tag_bits, bool delay_queue_enabled,
      unsigned int delay_queue_size, unsigned int delay_ticks, bool cross_page,
      bool adapt_offset, bool issue_validation, bool pc_validation_confidence,
      bool pc_validation_producer_consumer,
      bool global_coverage_guard, unsigned int pc_validation_entries,
      unsigned int pc_validation_tag_bits,
      unsigned int pc_validation_counter_bits,
      unsigned int pc_validation_initial,
      unsigned int pc_validation_medium_threshold,
      unsigned int pc_validation_high_threshold,
      unsigned int pc_validation_hit_increment,
      unsigned int pc_validation_medium_sample_period,
      unsigned int pc_validation_miss_decay_period,
      unsigned int pc_validation_low_entry_miss_streak_threshold,
      unsigned int pc_validation_epoch_bits,
      unsigned int pc_validation_offset_context_slots,
      unsigned int global_bop_unused_threshold,
      unsigned int global_bop_min_resolved_coverage_shift,
      bool negative_offsets_enable, bool auto_learning,
      unsigned int victim_offsets_list_size, unsigned int restore_cycle,
      Tick clock_period_ticks,
      const std::string &offsets);
    void bopReplayPhaseTraceWrite(
      uint64_t phase_id, const char *phase_name, Tick start_tick);
    void bopReplayDemandTraceWrite(
      uint64_t access_seq, Tick tick, Addr addr, Addr pc, bool has_pc,
      bool cache_miss, int prefetch_source, bool pf_first_hit, bool pf_hit);
    void bopReplayEventTraceWrite(
      uint64_t access_seq, uint64_t replay_order, Tick tick,
      const char *bop_name, const char *bop_kind,
      Addr trigger_addr, Addr trigger_pc, bool trigger_has_pc,
      bool trigger_is_demand, bool trigger_is_read, bool trigger_cache_miss,
      int trigger_pf_source, bool trigger_pf_first_hit, bool trigger_pf_hit,
      bool late, int64_t best_offset_before, int64_t best_offset_after,
      unsigned int best_score, unsigned int round, bool best_offset_changed,
      bool issue_enabled, bool validation_enabled, int validation_hit,
      bool pc_confidence_enabled, int pc_index, Addr pc_tag,
      int pc_entry_hit, int pc_confidence, int pc_state, bool pc_sampled,
      int pc_low_entry_miss_streak, int pc_epoch, bool global_bypass_active,
      bool policy_suppressed, bool raw_candidate_valid,
      Addr raw_candidate_addr, bool policy_candidate_valid,
      Addr policy_candidate_addr, Addr validation_addr, Addr prefetch_addr,
      bool online_generated, bool online_buffered, bool online_filtered,
      bool online_filter_passed);
    void bopReplayDelayActionTraceWrite(
      const char *bop_name, uint64_t replay_order, const char *action,
      Tick tick, Addr addr, Tick process_tick, unsigned int queue_size_after);
    void smsTrainTraceWrite(Tick tick, Addr old_addr, Addr cur_addr, Addr trigger_offset, int conf, bool miss);
    void strideTraceWrite(Tick tick, Addr addr, Addr PC, Addr hashPC, bool hit, bool isFirstShot, bool miss, bool is_train);
    void despacitoTraceWrite(Tick tick, Addr vaddr, Addr paddr, Addr PC, bool hasPC, bool miss, bool is_train);
    void dcacheWayPreTrace(Tick tick, uint64_t pc, uint64_t vaddr, int way, int is_write);
    void vaddrTrace(Tick tick, uint64_t pc, uint64_t vaddr, int hit);
    char memTraceSQLBuf[4096];
};


} // namespace gem5

#endif
