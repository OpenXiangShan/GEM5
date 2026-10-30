
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
    dumpBopReplayTrace(p.dump_bop_replay_trace),
    dumpSMSTrainTrace(p.dump_sms_train_trace),
    dumpStrideTrainTrace(p.dump_stride_train_trace),
    dumpDespacitoTrainTrace(p.dump_despacito_train_trace),
    dumpL1WayPreTrace(p.dump_l1d_way_pre_trace),
    dumpVaddrTrace(p.dump_vaddr_trace),
    dumpLifetime(p.dump_lifetime),
    mem_db(nullptr), zErrMsg(nullptr),rc(0),
    db_path(p.arch_db_file),
    bopReplayPhaseId(0), bopReplayAccessSeq(0), bopReplayEventSeq(0)
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
ArchDBer::bopReplayMetaTraceWrite(
    const char *bop_name, unsigned int block_size, unsigned int score_max,
    unsigned int round_max, unsigned int bad_score, unsigned int rr_entries,
    unsigned int tag_bits, bool delay_queue_enabled,
    unsigned int delay_queue_size, Tick delay_ticks, bool cross_page,
    bool adapt_offset, bool negative_offsets_enabled, bool auto_learning,
    unsigned int victim_offsets_list_size, unsigned int restore_cycle,
    Tick clock_period_ticks, bool student_cover_enabled,
    unsigned int student_pool_size, unsigned int student_issue_top_n,
    double student_conf_alpha,
    double student_cov_threshold, unsigned int student_teacher_top_n,
    unsigned int student_filter_entries, const std::string &student_hash_mode,
    unsigned int student_hash_count, bool student_large_offset_priority,
    double student_large_offset_priority_coeff,
    bool student_delay_queue_enabled, unsigned int student_delay_queue_size,
    Tick student_delay_ticks, const std::string &offsets)
{
    if (!(dumpGlobal && dumpBopReplayTrace))
        return;

    const char *sql_format =
        "INSERT INTO BOPReplayMeta("
        "SchemaVersion,BOPName,BlockSize,ScoreMax,RoundMax,BadScore,"
        "RREntries,TagBits,DelayQueueEnabled,DelayQueueSize,DelayTicks,"
        "CrossPage,AdaptOffset,NegativeOffsetsEnabled,AutoLearning,"
        "VictimOffsetsListSize,RestoreCycle,ClockPeriodTicks,"
        "StudentCoverEnabled,StudentPoolSize,StudentIssueTopN,StudentConfAlpha,"
        "StudentCovThreshold,StudentTeacherTopN,StudentFilterEntries,"
        "StudentHashMode,StudentHashCount,StudentLargeOffsetPriority,"
        "StudentLargeOffsetPriorityCoeff,StudentDelayQueueEnabled,"
        "StudentDelayQueueSize,StudentDelayTicks,Offsets) VALUES("
        "7,%Q,%u,%u,%u,%u,%u,%u,%d,%u,%lld,%d,%d,%d,%d,%u,%u,%lld,"
        "%d,%u,%u,%f,%f,%u,%u,%Q,%u,%d,%f,%d,%u,%lld,%Q);";
    char *sql = sqlite3_mprintf(
        sql_format, bop_name, block_size, score_max, round_max, bad_score,
        rr_entries, tag_bits, delay_queue_enabled, delay_queue_size,
        sqliteSignedInt(delay_ticks), cross_page, adapt_offset,
        negative_offsets_enabled, auto_learning, victim_offsets_list_size,
        restore_cycle, sqliteSignedInt(clock_period_ticks),
        student_cover_enabled, student_pool_size, student_issue_top_n,
        student_conf_alpha,
        student_cov_threshold, student_teacher_top_n, student_filter_entries,
        student_hash_mode.c_str(), student_hash_count,
        student_large_offset_priority, student_large_offset_priority_coeff,
        student_delay_queue_enabled, student_delay_queue_size,
        sqliteSignedInt(student_delay_ticks), offsets.c_str());
    rc = sqlite3_exec(mem_db, sql, callback, nullptr, &zErrMsg);
    sqlite3_free(sql);
    fatal_if(rc != SQLITE_OK, "SQL error writing BOPReplayMeta: %s\n", zErrMsg);
}

uint64_t
ArchDBer::bopReplayDemandTraceWrite(
    Tick tick, Addr addr, Addr pc, bool has_pc, bool cache_miss,
    int prefetch_source, bool pf_first_hit, bool pf_hit)
{
    if (!(dumpGlobal && dumpBopReplayTrace))
        return 0;

    if (bopReplayPhaseId == 0) {
        bopReplayPhaseId = 1;
        bopReplayPhaseTraceWrite(bopReplayPhaseId, "run", tick);
    }
    const uint64_t access_seq = ++bopReplayAccessSeq;
    char *sql = sqlite3_mprintf(
        "INSERT INTO L2DemandTrace(AccessSeq,PhaseId,Tick,Addr,PC,HasPC,"
        "CacheMiss,PrefetchSource,PfFirstHit,PfHit) VALUES(%lld,%lld,%lld,"
        "%lld,%lld,%d,%d,%d,%d,%d);",
        sqliteSignedInt(access_seq), sqliteSignedInt(bopReplayPhaseId),
        sqliteSignedInt(tick), sqliteSignedInt(addr), sqliteSignedInt(pc),
        has_pc, cache_miss, prefetch_source, pf_first_hit, pf_hit);
    rc = sqlite3_exec(mem_db, sql, callback, nullptr, &zErrMsg);
    sqlite3_free(sql);
    fatal_if(rc != SQLITE_OK, "SQL error writing L2DemandTrace: %s\n", zErrMsg);
    return access_seq;
}

void
ArchDBer::bopReplayPhaseTraceWrite(
    uint64_t phase_id, const char *phase_name, Tick start_tick)
{
    if (!(dumpGlobal && dumpBopReplayTrace))
        return;
    char *sql = sqlite3_mprintf(
        "INSERT OR IGNORE INTO BOPReplayPhase(PhaseId,PhaseName,StartTick) "
        "VALUES(%lld,%Q,%lld);", sqliteSignedInt(phase_id), phase_name,
        sqliteSignedInt(start_tick));
    rc = sqlite3_exec(mem_db, sql, callback, nullptr, &zErrMsg);
    sqlite3_free(sql);
    fatal_if(rc != SQLITE_OK, "SQL error writing BOPReplayPhase: %s\n", zErrMsg);
}

void
ArchDBer::bopReplayEventTraceWrite(
    uint64_t access_seq, Tick tick, const char *bop_name,
    const char *bop_kind, Addr trigger_addr, Addr trigger_pc,
    bool trigger_has_pc, bool late, int64_t teacher_offset,
    bool teacher_issue_enabled, bool student_issue_enabled,
    bool student_selected_valid, bool student_selected_enable,
    int64_t student_selected_offset, int64_t selected_offset,
    bool issue_enabled, bool cqf_allowed, unsigned int candidate_rank,
    bool page_allowed)
{
    if (!(dumpGlobal && dumpBopReplayTrace) || access_seq == 0)
        return;
    const uint64_t replay_order = ++bopReplayEventSeq;
    char *sql = sqlite3_mprintf(
        "INSERT INTO BOPReplayEvent(AccessSeq,BOPName,BOPKind,ReplayOrder,"
        "PhaseId,Tick,TriggerAddr,TriggerPC,TriggerHasPC,Late,"
        "TeacherOffset,TeacherIssueEnabled,StudentIssueEnabled,"
        "StudentSelectedValid,StudentSelectedEnable,StudentSelectedOffset,"
        "SelectedOffset,IssueEnabled,CQFAllowed,CandidateRank,PageAllowed) "
        "VALUES(%lld,%Q,%Q,%lld,"
        "%lld,%lld,%lld,%lld,%d,%d,%lld,%d,%d,%d,%d,%lld,%lld,%d,%d,%u,%d);",
        sqliteSignedInt(access_seq), bop_name, bop_kind,
        sqliteSignedInt(replay_order), sqliteSignedInt(bopReplayPhaseId),
        sqliteSignedInt(tick), sqliteSignedInt(trigger_addr),
        sqliteSignedInt(trigger_pc), trigger_has_pc, late,
        sqliteSignedInt(static_cast<uint64_t>(teacher_offset)),
        teacher_issue_enabled, student_issue_enabled, student_selected_valid,
        student_selected_enable,
        sqliteSignedInt(static_cast<uint64_t>(student_selected_offset)),
        sqliteSignedInt(static_cast<uint64_t>(selected_offset)), issue_enabled,
        cqf_allowed, candidate_rank, page_allowed);
    rc = sqlite3_exec(mem_db, sql, callback, nullptr, &zErrMsg);
    sqlite3_free(sql);
    fatal_if(rc != SQLITE_OK, "SQL error writing BOPReplayEvent: %s\n", zErrMsg);
}

void
ArchDBer::bopReplayDelayActionTraceWrite(
    const char *bop_name, const char *action, Tick tick, Addr addr,
    Tick process_tick, unsigned int queue_size_after)
{
    if (!(dumpGlobal && dumpBopReplayTrace))
        return;
    char *sql = sqlite3_mprintf(
        "INSERT INTO BOPReplayDelayAction(BOPName,Action,Tick,Addr,"
        "ProcessTick,QueueSizeAfter) VALUES(%Q,%Q,%lld,%lld,%lld,%u);",
        bop_name, action, sqliteSignedInt(tick), sqliteSignedInt(addr),
        sqliteSignedInt(process_tick), queue_size_after);
    rc = sqlite3_exec(mem_db, sql, callback, nullptr, &zErrMsg);
    sqlite3_free(sql);
    fatal_if(rc != SQLITE_OK,
             "SQL error writing BOPReplayDelayAction: %s\n", zErrMsg);
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
