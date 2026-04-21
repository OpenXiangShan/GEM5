
#include "sim/arch_db.hh"

#include <string>

#include "params/ArchDBer.hh"

namespace gem5{

namespace
{

long long
sqliteSignedInt(uint64_t value)
{
    return static_cast<long long>(static_cast<int64_t>(value));
}

std::string
sqlEscape(const char *text)
{
    if (!text) {
        return "";
    }

    std::string escaped;
    escaped.reserve(std::strlen(text));
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p == '\'') {
            escaped += "''";
        } else {
            escaped += *p;
        }
    }
    return escaped;
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
    dumpSMSTrainTrace(p.dump_sms_train_trace),
    dumpStrideTrainTrace(p.dump_stride_train_trace),
    dumpStrideOrderTrace(p.dump_stride_order_trace),
    dumpStrideDepthCtrlTrace(p.dump_stride_depth_ctrl_trace),
    dumpTrainFilterTrace(p.dump_train_filter_trace),
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
                        uint64_t completed, uint64_t committed, uint64_t writenback, int pf_src, uint64_t seqNum)
{
  bool dump_me = dumpGlobal && dumpMemTrace;
  if (!dump_me) return;

  sprintf(
      memTraceSQLBuf,
      "INSERT INTO MemTrace("
      "Tick,SeqNum,IsLoad,PC,VADDR,PADDR,Issued,Translated,"
      "Completed,Committed,Writenback,PFSrc,SITE) "
      "VALUES(%lld,%lld,%d,%lld,%lld,%lld,%lld,%lld,%lld,"
      "%lld,%lld,%d,'%s');",
      sqliteSignedInt(tick), sqliteSignedInt(seqNum), is_load, sqliteSignedInt(pc),
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
ArchDBer::strideTraceWrite(Tick tick, Addr addr, Addr PC, Addr hashPC, bool hit,
                           bool isFirstShot, bool miss, bool is_train,
                           uint64_t triggerSeqNum)
{
  bool dump_me = dumpGlobal && dumpStrideTrainTrace;
  if (!dump_me) return;

  sprintf(memTraceSQLBuf,
          "INSERT INTO StrideTrainTrace(Tick,Addr,PC,HashPC,QueryHit,IsFirstShot,Miss,IsTrain,TriggerSeqNum,SITE) "
          "VALUES(%lld,%lld,%lld,%lld,%d,%d,%d,%d,%lld,'%s');",
          sqliteSignedInt(tick), sqliteSignedInt(addr), sqliteSignedInt(PC),
          sqliteSignedInt(hashPC), hit, isFirstShot, miss, is_train,
          sqliteSignedInt(triggerSeqNum), "StrideTrain");
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

void
ArchDBer::strideOrderTraceWrite(Tick tick, const char *stage, uint64_t seqNum,
                                Addr pc, Addr addr, Addr blockAddr,
                                bool isLoad, bool miss, int pfSource,
                                int pfDepth, Tick observedTick,
                                int queueSize, const char *reason)
{
  bool dump_me = dumpGlobal && dumpStrideOrderTrace;
  if (!dump_me) return;

  sprintf(memTraceSQLBuf,
          "INSERT INTO StrideOrderTrace("
          "Tick,Stage,SeqNum,PC,Addr,BlockAddr,IsLoad,Miss,"
          "PfSource,PfDepth,ObservedTick,QueueSize,Reason,SITE) "
          "VALUES(%lld,'%s',%lld,%lld,%lld,%lld,%d,%d,%d,%d,"
          "%lld,%d,'%s','%s');",
          sqliteSignedInt(tick), stage, sqliteSignedInt(seqNum),
          sqliteSignedInt(pc), sqliteSignedInt(addr),
          sqliteSignedInt(blockAddr), isLoad, miss, pfSource, pfDepth,
          sqliteSignedInt(observedTick), queueSize, reason, "StrideOrder");
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

void
ArchDBer::strideDepthFeedbackTraceWrite(
    Tick tick, int level, const char *eventKind, int pfSource,
    int pfDepth, int aheadLevel, int globalL1Depth, int globalL2Gap,
    int effectiveL2Depth, uint64_t lateStrongWindow,
    uint64_t lateMSHRWindow, uint64_t lateCacheWindow,
    uint64_t timelyWindow, const char *site)
{
  bool dump_me = dumpGlobal && dumpStrideDepthCtrlTrace;
  if (!dump_me) return;

  const std::string sql =
      "INSERT INTO StrideDepthFeedbackTrace("
      "Tick,Level,EventKind,PfSource,PfDepth,AheadLevel,"
      "GlobalL1Depth,GlobalL2Gap,EffectiveL2Depth,LateStrongWindow,"
      "LateMSHRWindow,LateCacheWindow,TimelyWindow,SITE) VALUES(" +
      std::to_string(sqliteSignedInt(tick)) + "," +
      std::to_string(level) + ",'" +
      sqlEscape(eventKind) + "'," +
      std::to_string(pfSource) + "," +
      std::to_string(pfDepth) + "," +
      std::to_string(aheadLevel) + "," +
      std::to_string(globalL1Depth) + "," +
      std::to_string(globalL2Gap) + "," +
      std::to_string(effectiveL2Depth) + "," +
      std::to_string(sqliteSignedInt(lateStrongWindow)) + "," +
      std::to_string(sqliteSignedInt(lateMSHRWindow)) + "," +
      std::to_string(sqliteSignedInt(lateCacheWindow)) + "," +
      std::to_string(sqliteSignedInt(timelyWindow)) + ",'" +
      sqlEscape(site) + "');";
  execmd(sql);
}

void
ArchDBer::strideDepthDecisionTraceWrite(
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
    const char *site)
{
  bool dump_me = dumpGlobal && dumpStrideDepthCtrlTrace;
  if (!dump_me) return;

  const std::string sql =
      "INSERT INTO StrideDepthDecisionTrace("
      "Tick,Level,EvalSite,TotalFeedback,FeedbackWindow,WeightedLate,"
      "WeightedTotal,Action,Reason,OldL1Depth,OldL2Gap,OldEffectiveL2Depth,"
      "NewL1Depth,NewL2Gap,NewEffectiveL2Depth,PreLateStrongWindow,"
      "PreLateMSHRWindow,PreLateCacheWindow,PreTimelyWindow,"
      "PostLateStrongWindow,PostLateMSHRWindow,PostLateCacheWindow,"
      "PostTimelyWindow,StrongLateWeight,MSHRLateWeight,CacheLateWeight,"
      "RaiseThresholdPct,LowerWeakLatePct,LowerTimelyPct,SITE) VALUES(" +
      std::to_string(sqliteSignedInt(tick)) + "," +
      std::to_string(level) + ",'" +
      sqlEscape(evalSite) + "'," +
      std::to_string(sqliteSignedInt(totalFeedback)) + "," +
      std::to_string(sqliteSignedInt(feedbackWindow)) + "," +
      std::to_string(sqliteSignedInt(weightedLate)) + "," +
      std::to_string(sqliteSignedInt(weightedTotal)) + ",'" +
      sqlEscape(action) + "','" +
      sqlEscape(reason) + "'," +
      std::to_string(oldL1Depth) + "," +
      std::to_string(oldL2Gap) + "," +
      std::to_string(oldEffectiveL2Depth) + "," +
      std::to_string(newL1Depth) + "," +
      std::to_string(newL2Gap) + "," +
      std::to_string(newEffectiveL2Depth) + "," +
      std::to_string(sqliteSignedInt(preLateStrongWindow)) + "," +
      std::to_string(sqliteSignedInt(preLateMSHRWindow)) + "," +
      std::to_string(sqliteSignedInt(preLateCacheWindow)) + "," +
      std::to_string(sqliteSignedInt(preTimelyWindow)) + "," +
      std::to_string(sqliteSignedInt(postLateStrongWindow)) + "," +
      std::to_string(sqliteSignedInt(postLateMSHRWindow)) + "," +
      std::to_string(sqliteSignedInt(postLateCacheWindow)) + "," +
      std::to_string(sqliteSignedInt(postTimelyWindow)) + "," +
      std::to_string(sqliteSignedInt(strongLateWeight)) + "," +
      std::to_string(sqliteSignedInt(mshrLateWeight)) + "," +
      std::to_string(sqliteSignedInt(cacheLateWeight)) + "," +
      std::to_string(sqliteSignedInt(raiseThresholdPct)) + "," +
      std::to_string(sqliteSignedInt(lowerWeakLatePct)) + "," +
      std::to_string(sqliteSignedInt(lowerTimelyPct)) + ",'" +
      sqlEscape(site) + "');";
  execmd(sql);
}

void
ArchDBer::trainFilterTraceWrite(Tick tick, const char *stage, uint64_t seqNum,
                                Addr pc, Addr addr, Addr blockAddr,
                                bool isLoad, bool miss, int pfSource,
                                int pfDepth, Tick observedTick,
                                int queueSize, const char *reason)
{
  bool dump_me = dumpGlobal && dumpTrainFilterTrace;
  if (!dump_me) return;

  sprintf(memTraceSQLBuf,
          "INSERT INTO TrainFilterTrace("
          "Tick,Stage,SeqNum,PC,Addr,BlockAddr,IsLoad,Miss,"
          "PfSource,PfDepth,ObservedTick,QueueSize,Reason,SITE) "
          "VALUES(%lld,'%s',%lld,%lld,%lld,%lld,%d,%d,%d,%d,"
          "%lld,%d,'%s','%s');",
          sqliteSignedInt(tick), stage, sqliteSignedInt(seqNum),
          sqliteSignedInt(pc), sqliteSignedInt(addr),
          sqliteSignedInt(blockAddr), isLoad, miss, pfSource, pfDepth,
          sqliteSignedInt(observedTick), queueSize, reason, "TrainFilter");
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

void
ArchDBer::loadOrderTraceWrite(Tick tick, const char *stage, uint64_t seqNum,
                              Addr pc, Addr vaddr, Addr paddr,
                              const char *reason)
{
  bool dump_me = dumpGlobal && dumpTrainFilterTrace;
  if (!dump_me) return;

  sprintf(memTraceSQLBuf,
          "INSERT INTO LoadOrderTrace("
          "Tick,Stage,SeqNum,PC,VAddr,PAddr,Reason,SITE) "
          "VALUES(%lld,'%s',%lld,%lld,%lld,%lld,'%s','%s');",
          sqliteSignedInt(tick), stage, sqliteSignedInt(seqNum),
          sqliteSignedInt(pc), sqliteSignedInt(vaddr),
          sqliteSignedInt(paddr), reason, "LoadOrder");
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
          "VALUES(%ld,%ld,%ld,%ld,%d,%d,%d,'%s');",
          tick, vaddr, paddr, PC, hasPC, miss, is_train, is_train?"DespacitoTrain":"DespacitoPrefetch");
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
