
#include "sim/arch_db.hh"

#include "params/ArchDBer.hh"

namespace gem5{

namespace
{

long long
sqliteSignedInt(uint64_t value)
{
    return static_cast<long long>(static_cast<int64_t>(value));
}

} // anonymous namespace

ArchDBer::ArchDBer(const Params &p)
    : SimObject(p), dumpGlobal(p.dump_from_start),
    dumpRolling(p.enable_rolling),
    dumpMemTrace(p.dump_mem_trace),
    dumpL1PfTrace(p.dump_l1_pf_trace),
    dumpL1EvictTrace(p.dump_l1_evict_trace),
    dumpL2EvictTrace(p.dump_l2_evict_trace),
    dumpL3EvictTrace(p.dump_l3_evict_trace),
    dumpL1MissTrace(p.dump_l1_miss_trace),
    dumpBopTrainTrace(p.dump_bop_train_trace),
    dumpBopValidationTrace(p.dump_bop_validation_trace),
    dumpSMSTrainTrace(p.dump_sms_train_trace),
    dumpStrideTrainTrace(p.dump_stride_train_trace),
    dumpDespacitoTrainTrace(p.dump_despacito_train_trace),
    dumpL1WayPreTrace(p.dump_l1d_way_pre_trace),
    dumpVaddrTrace(p.dump_vaddr_trace),
    dumpLifetime(p.dump_lifetime),
    mem_db(nullptr), zErrMsg(nullptr),rc(0),
    db_path(p.arch_db_file)
{
  int rc = sqlite3_open(":memory:", &mem_db);
  if (rc) {
    sqlite3_close(mem_db);
    fatal("Can't open database: %s\n", sqlite3_errmsg(mem_db));
  }

  fatal_if(db_path == "" || db_path == "None",
            "Arch db file path is not given!");

  for (const auto &s : p.table_cmds) {
    create_table(s);
  }
  registerExitCallback([this](){ save_db(); });
}

static int callback(void *NotUsed, int argc, char **argv, char **azColName){
  return 0;
}

void ArchDBer::create_table(const std::string &sql) {
  // create table
  rc = sqlite3_exec(mem_db, sql.c_str(), callback, 0, &zErrMsg);
  fatal_if(rc != SQLITE_OK, "SQL error: %s\n", zErrMsg);
  inform("Table created: %s\n", sql.c_str());
}

void ArchDBer::start_recording() {
  dumpGlobal = true;
}

void ArchDBer::save_db() {
  warn("saving memdb to %s ...\n", db_path.c_str());
  sqlite3 *disk_db;
  sqlite3_backup *pBackup;
  int rc = sqlite3_open(db_path.c_str(), &disk_db);
  if (rc == SQLITE_OK){
    pBackup = sqlite3_backup_init(disk_db, "main", mem_db, "main");
    if (pBackup){
      (void)sqlite3_backup_step(pBackup, -1);
      (void)sqlite3_backup_finish(pBackup);
    }
    rc = sqlite3_errcode(disk_db);
  }
  sqlite3_close(disk_db);
}

void
ArchDBer::execmd(std::string cmd)
{
  rc = sqlite3_exec(mem_db, cmd.c_str(), callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  }
}

DBTraceManager *
ArchDBer::addAndGetTrace(const char *name, std::vector<std::pair<std::string, DataType>> fields)
{
  _traces[name] = DBTraceManager(name, fields, mem_db);
  return &_traces[name];
}

void
ArchDBer::memTraceWrite(Tick tick, bool is_load, Addr pc, Addr vaddr, Addr paddr, uint64_t issued, uint64_t translated,
                        uint64_t completed, uint64_t committed, uint64_t writenback, int pf_src)
{
  bool dump_me = dumpGlobal && dumpMemTrace;
  if (!dump_me) return;

  sprintf(
      memTraceSQLBuf,
      "INSERT INTO MemTrace(Tick,IsLoad,PC,VADDR,PADDR,Issued,Translated,Completed,Committed,Writenback,PFSrc,SITE) "
      "VALUES(%lld,%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%d,'%s');",
      sqliteSignedInt(tick), is_load, sqliteSignedInt(pc),
      sqliteSignedInt(vaddr), sqliteSignedInt(paddr),
      sqliteSignedInt(issued), sqliteSignedInt(translated),
      sqliteSignedInt(completed), sqliteSignedInt(committed),
      sqliteSignedInt(writenback), pf_src, "CommitMemTrace");
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

void
ArchDBer::l1PFTraceWrite(Tick tick, Addr trigger_pc, Addr trigger_vaddr, Addr pf_vaddr, int pf_src)
{
  bool dump_me = dumpGlobal && dumpL1PfTrace;
  if (!dump_me) return;

  sprintf(memTraceSQLBuf,
          "INSERT INTO L1PFTrace(Tick,TriggerPC,TriggerVAddr,PFVAddr,PFSrc,SITE) "
          "VALUES(%lld,%lld,%lld,%lld,%d,'%s');",
          sqliteSignedInt(tick), sqliteSignedInt(trigger_pc),
          sqliteSignedInt(trigger_vaddr),
          sqliteSignedInt(pf_vaddr), pf_src, "L1PFTrace");
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

void
ArchDBer::bopTrainTraceWrite(Tick tick, Addr old_addr, Addr cur_addr, Addr offset, int score, bool miss)
{
  bool dump_me = dumpGlobal && dumpBopTrainTrace;
  if (!dump_me) return;

  sprintf(memTraceSQLBuf,
          "INSERT INTO BOPTrainTrace(Tick,OldAddr,CurAddr,Offset,Score,Miss,SITE) "
          "VALUES(%lld,%lld,%lld,%lld,%d,%d,'%s');",
          sqliteSignedInt(tick), sqliteSignedInt(old_addr),
          sqliteSignedInt(cur_addr), sqliteSignedInt(offset), score, miss, "BOPTrain");
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

void
ArchDBer::bopValidationTraceWrite(
    Tick tick, const char *event, const char *bop_name,
    Addr trigger_pc, Addr trigger_addr, Addr validation_addr, Addr pf_addr,
    int64_t best_offset, int best_score, int round, bool late,
    bool trigger_is_demand, bool trigger_cache_miss, int trigger_pf_source,
    bool trigger_pf_first_hit, bool trigger_pf_hit, int issue_enabled,
    int validation_enabled, int validation_hit, bool suppressed,
    bool generated, bool buffered, bool filtered, bool filter_passed,
    bool pc_confidence_enabled, int pc_index, Addr pc_tag,
    int pc_entry_hit, int pc_confidence, int pc_state,
    bool pc_sampled, int pc_epoch, int pc_low_entry_miss_streak)
{
  if (!(dumpGlobal && dumpBopValidationTrace)) return;

  sprintf(
      memTraceSQLBuf,
      "INSERT INTO BOPValidationTrace("
      "Tick,Event,BOPName,TriggerPC,TriggerAddr,ValidationAddr,PrefetchAddr,"
      "BestOffset,BestScore,Round,Late,TriggerIsDemand,TriggerCacheMiss,"
      "TriggerPFSource,TriggerPFFirstHit,TriggerPFHit,IssueEnabled,"
      "ValidationEnabled,ValidationHit,PCConfidenceEnabled,PCIndex,PCTag,"
      "PCEntryHit,PCConfidence,PCState,PCSampled,PCEpoch,Suppressed,"
      "Generated,Buffered,Filtered,FilterPassed,PCConfidenceAfter,"
      "PCUpdateDecayed,PCUpdateParticipants,PCOffsetChanged,OutcomeAddr,"
      "OutcomePC,OutcomePFSource,OutcomeIsDemand,OutcomeCacheMiss,SITE,"
      "PCLowEntryMissStreak,PCUpdateLowEntryMissStreakBefore,"
      "PCUpdateLowEntryMissStreakAfter,PCUpdateLowEntryHysteresisHeld,"
      "PCUpdateLowEntryHysteresisTransition) "
      "VALUES(%lld,'%s','%s',%lld,%lld,%lld,%lld,%lld,%d,%d,%d,%d,"
      "%d,%d,%d,%d,%d,%d,%d,%d,%d,%lld,%d,%d,%d,%d,%d,%d,%d,%d,"
      "%d,%d,%d,%d,%d,%d,%lld,%lld,%d,%d,%d,'%s',%d,%d,%d,%d,%d);",
      sqliteSignedInt(tick), event, bop_name,
      sqliteSignedInt(trigger_pc), sqliteSignedInt(trigger_addr),
      sqliteSignedInt(validation_addr), sqliteSignedInt(pf_addr),
      sqliteSignedInt(static_cast<uint64_t>(best_offset)), best_score, round,
      late, trigger_is_demand, trigger_cache_miss, trigger_pf_source,
      trigger_pf_first_hit, trigger_pf_hit, issue_enabled,
      validation_enabled, validation_hit, pc_confidence_enabled, pc_index,
      sqliteSignedInt(pc_tag), pc_entry_hit, pc_confidence, pc_state,
      pc_sampled, pc_epoch, suppressed, generated, buffered, filtered,
      filter_passed, -1, 0, 0, 0, 0LL, 0LL, 0, 0, 0, "BOPValidation",
      pc_low_entry_miss_streak, -1, -1, 0, 0);
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  }
}

void
ArchDBer::bopValidationConfidenceUpdateTraceWrite(
    Tick tick, const char *bop_name, Addr trigger_pc, unsigned int pc_index,
    Addr pc_tag, bool validation_hit, unsigned int participants,
    int confidence_before, int confidence_after, bool decayed,
    bool offset_changed, unsigned int epoch_after,
    int low_entry_miss_streak_before, int low_entry_miss_streak_after,
    bool low_entry_hysteresis_held, bool low_entry_hysteresis_transition)
{
  if (!(dumpGlobal && dumpBopValidationTrace)) return;

  sprintf(
      memTraceSQLBuf,
      "INSERT INTO BOPValidationTrace("
      "Tick,Event,BOPName,TriggerPC,TriggerAddr,ValidationAddr,PrefetchAddr,"
      "BestOffset,BestScore,Round,Late,TriggerIsDemand,TriggerCacheMiss,"
      "TriggerPFSource,TriggerPFFirstHit,TriggerPFHit,IssueEnabled,"
      "ValidationEnabled,ValidationHit,PCConfidenceEnabled,PCIndex,PCTag,"
      "PCEntryHit,PCConfidence,PCState,PCSampled,PCEpoch,Suppressed,"
      "Generated,Buffered,Filtered,FilterPassed,PCConfidenceAfter,"
      "PCUpdateDecayed,PCUpdateParticipants,PCOffsetChanged,OutcomeAddr,"
      "OutcomePC,OutcomePFSource,OutcomeIsDemand,OutcomeCacheMiss,SITE,"
      "PCLowEntryMissStreak,PCUpdateLowEntryMissStreakBefore,"
      "PCUpdateLowEntryMissStreakAfter,PCUpdateLowEntryHysteresisHeld,"
      "PCUpdateLowEntryHysteresisTransition) "
      "VALUES(%lld,'confidence_update','%s',%lld,0,0,0,0,0,0,0,0,"
      "0,0,0,0,1,1,%d,1,%u,%lld,1,%d,-1,0,%u,0,0,0,0,0,%d,%d,"
      "%u,%d,0,0,0,0,0,'%s',-1,%d,%d,%d,%d);",
      sqliteSignedInt(tick), bop_name, sqliteSignedInt(trigger_pc),
      validation_hit, pc_index, sqliteSignedInt(pc_tag), confidence_before,
      epoch_after, confidence_after, decayed, participants, offset_changed,
      "BOPValidationConfidence", low_entry_miss_streak_before,
      low_entry_miss_streak_after, low_entry_hysteresis_held,
      low_entry_hysteresis_transition);
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  }
}

void
ArchDBer::bopValidationOutcomeTraceWrite(
    Tick tick, const char *event, Addr addr, Addr pc, int pf_source,
    bool is_demand, bool cache_miss)
{
  if (!(dumpGlobal && dumpBopValidationTrace)) return;

  sprintf(
      memTraceSQLBuf,
      "INSERT INTO BOPValidationTrace("
      "Tick,Event,BOPName,TriggerPC,TriggerAddr,ValidationAddr,PrefetchAddr,"
      "BestOffset,BestScore,Round,Late,TriggerIsDemand,TriggerCacheMiss,"
      "TriggerPFSource,TriggerPFFirstHit,TriggerPFHit,IssueEnabled,"
      "ValidationEnabled,ValidationHit,PCConfidenceEnabled,PCIndex,PCTag,"
      "PCEntryHit,PCConfidence,PCState,PCSampled,PCEpoch,Suppressed,"
      "Generated,Buffered,Filtered,FilterPassed,PCConfidenceAfter,"
      "PCUpdateDecayed,PCUpdateParticipants,PCOffsetChanged,OutcomeAddr,"
      "OutcomePC,OutcomePFSource,OutcomeIsDemand,OutcomeCacheMiss,SITE,"
      "PCLowEntryMissStreak,PCUpdateLowEntryMissStreakBefore,"
      "PCUpdateLowEntryMissStreakAfter,PCUpdateLowEntryHysteresisHeld,"
      "PCUpdateLowEntryHysteresisTransition) "
      "VALUES(%lld,'%s','L2BOP',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
      "-1,0,-1,0,-1,-1,-1,0,-1,0,0,0,0,0,-1,0,0,0,%lld,%lld,"
      "%d,%d,%d,'%s',-1,-1,-1,0,0);",
      sqliteSignedInt(tick), event, sqliteSignedInt(addr), sqliteSignedInt(pc),
      pf_source, is_demand, cache_miss, "BOPValidationOutcome");
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  }
}

void
ArchDBer::bopValidationLiveRecordTraceWrite(
    Tick tick, const char *event, Addr line_addr, bool secure,
    const char *attempt_bop_name, const char *attempt_kind, Addr attempt_pc,
    const char *owner_bop_name, const char *owner_kind, Addr owner_pc,
    Addr first_consumer_pc, bool consumed, bool found, bool created,
    bool duplicate_issue)
{
  if (!(dumpGlobal && dumpBopValidationTrace)) return;

  sprintf(
      memTraceSQLBuf,
      "INSERT INTO BOPLiveRecordTrace("
      "Tick,Event,LineAddr,Secure,AttemptBOPName,AttemptKind,AttemptPC,"
      "OwnerBOPName,OwnerKind,OwnerPC,FirstConsumerPC,Consumed,Found,"
      "Created,DuplicateIssue,SITE) "
      "VALUES(%lld,'%s',%lld,%d,'%s','%s',%lld,'%s','%s',%lld,%lld,"
      "%d,%d,%d,%d,'%s');",
      sqliteSignedInt(tick), event, sqliteSignedInt(line_addr), secure,
      attempt_bop_name, attempt_kind, sqliteSignedInt(attempt_pc),
      owner_bop_name, owner_kind, sqliteSignedInt(owner_pc),
      sqliteSignedInt(first_consumer_pc), consumed, found, created,
      duplicate_issue, "BOPLiveRecord");
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  }
}

void
ArchDBer::smsTrainTraceWrite(Tick tick, Addr old_addr, Addr cur_addr, Addr trigger_offset, int conf, bool miss)
{
  bool dump_me = dumpGlobal && dumpSMSTrainTrace;
  if (!dump_me) return;

  sprintf(memTraceSQLBuf,
          "INSERT INTO SMSTrainTrace(Tick,OldAddr,CurAddr,TriggerOffset,Conf,Miss,SITE) "
          "VALUES(%lld,%lld,%lld,%lld,%d,%d,'%s');",
          sqliteSignedInt(tick), sqliteSignedInt(old_addr),
          sqliteSignedInt(cur_addr),
          sqliteSignedInt(trigger_offset), conf, miss, "SMSTrain");
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}
void
ArchDBer::strideTraceWrite(Tick tick, Addr addr, Addr PC, Addr hashPC, bool hit, bool isFirstShot, bool miss, bool is_train)
{
  bool dump_me = dumpGlobal && dumpStrideTrainTrace;
  if (!dump_me) return;

  sprintf(memTraceSQLBuf,
          "INSERT INTO StrideTrainTrace(Tick,Addr,PC,HashPC,QueryHit,IsFirstShot,Miss,IsTrain,SITE) "
          "VALUES(%lld,%lld,%lld,%lld,%d,%d,%d,%d,'%s');",
          sqliteSignedInt(tick), sqliteSignedInt(addr),
          sqliteSignedInt(PC), sqliteSignedInt(hashPC),
          hit, isFirstShot, miss, is_train, "StrideTrain");
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}
void
ArchDBer::despacitoTraceWrite(Tick tick, Addr vaddr, Addr paddr, Addr PC, bool hasPC, bool miss, bool is_train)
{
  bool dump_me = dumpGlobal && dumpDespacitoTrainTrace;
  if (!dump_me) return;

  sprintf(memTraceSQLBuf,
          "INSERT INTO DespacitoTrainTrace(Tick,vAddr,pAddr,PC,hasPC,Miss,IsTrain,SITE) "
          "VALUES(%lld,%lld,%lld,%lld,%d,%d,%d,'%s');",
          sqliteSignedInt(tick), sqliteSignedInt(vaddr),
          sqliteSignedInt(paddr), sqliteSignedInt(PC),
          hasPC, miss, is_train,
          is_train ? "DespacitoTrain" : "DespacitoPrefetch");
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}
void ArchDBer::L1MissTrace_write(
  uint64_t pc,
  uint64_t source,
  uint64_t paddr,
  uint64_t vaddr,
  uint64_t stamp,
  const char * site
) {
  bool dump_me = dumpGlobal && dumpL1MissTrace;
  if (!dump_me) return;
  char sql[512];
  sprintf(sql,
    "INSERT INTO L1MissTrace(PC,SOURCE,PADDR,VADDR, STAMP, SITE) " \
    "VALUES(%lld, %lld, %lld, %lld, %lld, '%s');",
    sqliteSignedInt(pc), sqliteSignedInt(source),
    sqliteSignedInt(paddr), sqliteSignedInt(vaddr),
    sqliteSignedInt(stamp), site
  );
  rc = sqlite3_exec(mem_db, sql, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

void
ArchDBer::dcacheWayPreTrace(Tick tick, uint64_t pc, uint64_t vaddr, int way, int is_write)
{
    bool dump_me = dumpGlobal && dumpL1WayPreTrace;
    if (!dump_me)
        return;
    char sql[512];
    sprintf(sql,
            "INSERT INTO dcacheWayPreTrace(PC,VADDR, WAY, Tick, IsWrite,SITE)"
            "VALUES(%lld,%lld,%d,%lld,%d,'%s');",
            sqliteSignedInt(pc), sqliteSignedInt(vaddr),
            way, sqliteSignedInt(tick),
            is_write, "dacheWayPre");
    rc = sqlite3_exec(mem_db, sql, callback, 0, &zErrMsg);
    if (rc != SQLITE_OK) {
        fatal("SQL error: %s\n", zErrMsg);
    };
}
void
ArchDBer::vaddrTrace(Tick tick, uint64_t pc, uint64_t vaddr, int hit)
{
    bool dump_me = dumpGlobal && dumpVaddrTrace;
    if (!dump_me)
        return;
    char sql[512];
    sprintf(sql,
            "INSERT INTO vaddrTrace(PC, VADDR, Hit, Tick, SITE)"
            "VALUES(%lld,%lld,%d,%lld,'%s');",
            sqliteSignedInt(pc), sqliteSignedInt(vaddr),
            hit, sqliteSignedInt(tick), "vaddrTrace");
    rc = sqlite3_exec(mem_db, sql, callback, 0, &zErrMsg);
    if (rc != SQLITE_OK) {
        fatal("SQL error: %s\n", zErrMsg);
    };
}

void
ArchDBer::evictTraceWrite(int cache_level, Tick tick, uint64_t paddr, uint64_t stamp, const char *site)
{
  bool dump_me = dumpGlobal && ((dumpL1EvictTrace && cache_level == 1) || (dumpL2EvictTrace && cache_level == 2) ||
                                (dumpL3EvictTrace && cache_level == 3));
  if (!dump_me) return;
  char sql[512];
  sprintf(sql,
    "INSERT INTO CacheEvictTrace(Tick, PADDR, STAMP, Level, SITE) " \
    "VALUES(%lld, %lld, %lld, %d, '%s');",
    sqliteSignedInt(tick), sqliteSignedInt(paddr),
    sqliteSignedInt(stamp), cache_level, site
  );
  rc = sqlite3_exec(mem_db, sql, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

void
DBTraceManager::init_table() {
  // create table
  char sql[1024];
  int pos = 0;
  pos = sprintf(sql,
    "CREATE TABLE %s(" \
    "ID INTEGER PRIMARY KEY AUTOINCREMENT, " \
    "TICK INT NOT NULL", _name.c_str());
  for (auto it = _fields.begin(); it != _fields.end(); it++) {
    switch (it->second) {
      case UINT64:
        pos += sprintf(sql+pos, ",%s INT NOT NULL", it->first.c_str());
        break;
      case TEXT:
        pos += sprintf(sql+pos, ",%s TEXT", it->first.c_str());
        break;
      default:
        fatal("Unknown data type");
    }
  }
  pos += sprintf(sql+pos, ");");
  assert(pos < 1024);
  printf("%s\n", sql);
  char *zErrMsg;
  int rc = sqlite3_exec(_db, sql, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  } else {
    warn("Table created: %s\n", _name.c_str());
  }
}

void
DBTraceManager::write_record(const Record &record)
{
  char sql[1024];
  int pos = 0;
  pos = sprintf(sql, "INSERT INTO %s(TICK", _name.c_str());
  for (auto it = _fields.begin(); it != _fields.end(); it++) {
    pos += sprintf(sql+pos, ",%s", it->first.c_str());
  }
  pos += sprintf(sql+pos, ") VALUES(%lld",
      sqliteSignedInt(record._tick));
  for (auto it = _fields.begin(); it != _fields.end(); it++) {
    switch (it->second) {
      case UINT64:
      {
        auto &m = record._uint64_data;
        auto data = m.find(it->first);
        if (data == m.end()) {
          fatal("Can't find data for %s\n", it->first.c_str());
        }
        assert(data != m.end());
        pos += sprintf(sql+pos, ",%lld",
            sqliteSignedInt(data->second));
        break;
      }
      case TEXT:
      {
        auto &m = record._text_data;
        auto data = m.find(it->first);
        if (data == m.end()) {
          fatal("Can't find data for %s\n", it->first.c_str());
        }
        assert(data != m.end());
        pos += sprintf(sql+pos, ",'%s'", data->second.c_str());
        break;
      }
      default:
        fatal("Unknown data type!\n");
    }
  }
  pos += sprintf(sql+pos, ");");
  assert(pos < 1024);
  char *zErrMsg;
  int rc = sqlite3_exec(_db, sql, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

} // namespace gem5
