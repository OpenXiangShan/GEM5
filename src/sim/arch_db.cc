
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

void
prepareStatement(sqlite3 *db, sqlite3_stmt **statement, const char *sql,
                 const char *name)
{
    const int result = sqlite3_prepare_v2(db, sql, -1, statement, nullptr);
    fatal_if(result != SQLITE_OK, "Failed to prepare %s: %s\n", name,
             sqlite3_errmsg(db));
}

void
stepAndReset(sqlite3 *db, sqlite3_stmt *statement, const char *name)
{
    const int step_result = sqlite3_step(statement);
    fatal_if(step_result != SQLITE_DONE, "Failed to write %s: %s\n", name,
             sqlite3_errmsg(db));

    const int reset_result = sqlite3_reset(statement);
    fatal_if(reset_result != SQLITE_OK, "Failed to reset %s: %s\n", name,
             sqlite3_errmsg(db));
    sqlite3_clear_bindings(statement);
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
    dumpBopReplayTrace(p.dump_bop_replay_trace),
    dumpBopDirectQualityTrace(p.dump_bop_direct_quality_trace),
    dumpSMSTrainTrace(p.dump_sms_train_trace),
    dumpStrideTrainTrace(p.dump_stride_train_trace),
    dumpDespacitoTrainTrace(p.dump_despacito_train_trace),
    dumpL1WayPreTrace(p.dump_l1d_way_pre_trace),
    dumpVaddrTrace(p.dump_vaddr_trace),
    dumpLifetime(p.dump_lifetime),
    mem_db(nullptr), bopReplayMetaStmt(nullptr), bopReplayPhaseStmt(nullptr),
    bopReplayDemandStmt(nullptr), bopReplayEventStmt(nullptr),
    bopReplayDelayActionStmt(nullptr),
    bopDirectQualityMetaStmt(nullptr), bopDirectQualityCandidateStmt(nullptr),
    bopDirectQualityIssueStmt(nullptr),
    bopDirectQualityDemandStmt(nullptr), bopDirectQualityOutcomeStmt(nullptr),
    bopReplayPhaseId(0), zErrMsg(nullptr),rc(0),
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
  if (dumpBopReplayTrace) {
    prepareStatement(
        mem_db, &bopReplayMetaStmt,
        "INSERT OR IGNORE INTO BOPReplayMeta("
        "SchemaVersion,BOPName,BlockSize,ScoreMax,RoundMax,BadScore,"
        "RREntries,TagBits,DelayQueueEnabled,DelayQueueSize,DelayTicks,"
        "CrossPage,AdaptOffset,IssueValidation,PCValidationConfidence,"
        "PCValidationProducerConsumer,GlobalCoverageGuard,"
        "PCValidationEntries,PCValidationTagBits,"
        "PCValidationCounterBits,PCValidationInitial,"
        "PCValidationMediumThreshold,PCValidationHighThreshold,"
        "PCValidationHitIncrement,PCValidationMediumSamplePeriod,"
        "PCValidationMissDecayPeriod,PCValidationLowEntryMissStreakThreshold,"
        "PCValidationEpochBits,PCValidationOffsetContextSlots,"
        "GlobalBOPUnusedThreshold,"
        "GlobalBOPMinResolvedCoverageShift,NegativeOffsetsEnabled,"
        "AutoLearning,VictimOffsetsListSize,RestoreCycle,"
        "ClockPeriodTicks,Offsets) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,"
        "?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,"
        "?25,?26,?27,?28,?29,?30,?31,?32,?33,?34,?35,?36,?37);",
        "BOPReplayMeta insert");
    prepareStatement(
        mem_db, &bopReplayPhaseStmt,
        "INSERT OR IGNORE INTO BOPReplayPhase(PhaseId,PhaseName,StartTick) "
        "VALUES(?,?,?);",
        "BOPReplayPhase insert");
    prepareStatement(
        mem_db, &bopReplayDemandStmt,
        "INSERT INTO L2DemandTrace("
        "AccessSeq,PhaseId,Tick,Addr,PC,HasPC,CacheMiss,PrefetchSource,"
        "PfFirstHit,PfHit) VALUES(?,?,?,?,?,?,?,?,?,?);",
        "L2DemandTrace insert");
    prepareStatement(
        mem_db, &bopReplayEventStmt,
        "INSERT INTO BOPReplayEvent("
        "AccessSeq,BOPName,BOPKind,ReplayOrder,PhaseId,Tick,TriggerAddr,TriggerPC,"
        "TriggerHasPC,"
        "TriggerIsDemand,TriggerIsRead,TriggerCacheMiss,TriggerPFSource,"
        "TriggerPFFirstHit,TriggerPFHit,Late,BestOffsetBefore,"
        "BestOffsetAfter,BestScore,Round,BestOffsetChanged,IssueEnabled,"
        "ValidationEnabled,ValidationHit,PCConfidenceEnabled,PCIndex,PCTag,"
        "PCEntryHit,PCConfidence,PCState,PCSampled,PCLowEntryMissStreak,"
        "PCEpoch,GlobalBypassActive,PolicySuppressed,RawCandidateValid,"
        "RawCandidateAddr,PolicyCandidateValid,PolicyCandidateAddr,"
        "ValidationAddr,PrefetchAddr,OnlineGenerated,OnlineBuffered,"
        "OnlineFiltered,OnlineFilterPassed) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,"
        "?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,"
        "?25,?26,?27,?28,?29,?30,?31,?32,?33,?34,?35,?36,"
        "?37,?38,?39,?40,?41,?42,?43,?44,?45);",
        "BOPReplayEvent insert");
    prepareStatement(
        mem_db, &bopReplayDelayActionStmt,
        "INSERT INTO BOPReplayDelayAction("
        "BOPName,ReplayOrder,Action,Tick,Addr,ProcessTick,QueueSizeAfter) "
        "VALUES(?,?,?,?,?,?,?);",
        "BOPReplayDelayAction insert");
    bopReplayPhaseTraceWrite(0, "trace_start", curTick());
  }
  if (dumpBopDirectQualityTrace) {
    prepareStatement(
        mem_db, &bopDirectQualityMetaStmt,
        "INSERT OR IGNORE INTO BOPDirectQualityMeta("
        "SchemaVersion,QualityEntries,QualityWays,QualityTagBits,"
        "FeedbackEntries,FeedbackWays,Horizon,MinSamples,"
        "ObserveSamplePeriod,OpenSamplePeriod,BlockProbePeriod,"
        "BorderlineBlockProbePeriod,UnusedPerUseful,BlockGuard,"
        "StrictUnusedPerUseful,StrictBlockGuard,ReopenUnusedPerUseful,"
        "ReopenGuard,ReopenProbePeriod,ReopenConfirmSamples,DecayPeriod) "
        "VALUES(3,?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,"
        "?16,?17,?18,?19,?20);",
        "BOPDirectQualityMeta insert");
    prepareStatement(
        mem_db, &bopDirectQualityCandidateStmt,
        "INSERT INTO BOPDirectQualityCandidate("
        "EventSequence,Tick,PC,Kind,TriggerLine,CandidateLine,State,"
        "Allowed,Sampled) VALUES(?,?,?,?,?,?,?,?,?);",
        "BOPDirectQualityCandidate insert");
    prepareStatement(
        mem_db, &bopDirectQualityIssueStmt,
        "INSERT INTO BOPDirectQualityIssue("
        "EventSequence,FeedbackId,IssueDemandSequence,Tick,Line,Kind) "
        "VALUES(?,?,?,?,?,?);",
        "BOPDirectQualityIssue insert");
    prepareStatement(
        mem_db, &bopDirectQualityDemandStmt,
        "INSERT INTO BOPDirectQualityDemand("
        "EventSequence,DemandSequence,Tick,Line) VALUES(?,?,?,?);",
        "BOPDirectQualityDemand insert");
    prepareStatement(
        mem_db, &bopDirectQualityOutcomeStmt,
        "INSERT INTO BOPDirectQualityOutcome("
        "EventSequence,FeedbackId,ResolveDemandSequence,Tick,Line,Outcome) "
        "VALUES(?,?,?,?,?,?);",
        "BOPDirectQualityOutcome insert");
  }
  registerExitCallback([this](){ save_db(); });
}

ArchDBer::~ArchDBer()
{
  sqlite3_finalize(bopReplayMetaStmt);
  sqlite3_finalize(bopReplayPhaseStmt);
  sqlite3_finalize(bopReplayDemandStmt);
  sqlite3_finalize(bopReplayEventStmt);
  sqlite3_finalize(bopReplayDelayActionStmt);
  sqlite3_finalize(bopDirectQualityMetaStmt);
  sqlite3_finalize(bopDirectQualityCandidateStmt);
  sqlite3_finalize(bopDirectQualityIssueStmt);
  sqlite3_finalize(bopDirectQualityDemandStmt);
  sqlite3_finalize(bopDirectQualityOutcomeStmt);
  sqlite3_close(mem_db);
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
  if (dumpBopReplayTrace && bopReplayPhaseId == 0) {
    bopReplayPhaseId = 1;
    bopReplayPhaseTraceWrite(bopReplayPhaseId, "stable", curTick());
  }
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
ArchDBer::bopReplayMetaTraceWrite(
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
    const std::string &offsets)
{
  if (!(dumpGlobal && dumpBopReplayTrace)) {
    return;
  }

  int column = 1;
  const auto bind = [this](int result) {
    fatal_if(result != SQLITE_OK, "Failed to bind BOPReplayMeta: %s\n",
             sqlite3_errmsg(mem_db));
  };
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, 5));
  bind(sqlite3_bind_text(bopReplayMetaStmt, column++, bop_name, -1,
                         SQLITE_TRANSIENT));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, block_size));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, score_max));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, round_max));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, bad_score));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, rr_entries));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, tag_bits));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, delay_queue_enabled));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, delay_queue_size));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, delay_ticks));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, cross_page));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, adapt_offset));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, issue_validation));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++,
                         pc_validation_confidence));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++,
                         pc_validation_producer_consumer));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, global_coverage_guard));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, pc_validation_entries));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, pc_validation_tag_bits));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++,
                         pc_validation_counter_bits));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, pc_validation_initial));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++,
                         pc_validation_medium_threshold));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++,
                         pc_validation_high_threshold));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++,
                         pc_validation_hit_increment));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++,
                         pc_validation_medium_sample_period));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++,
                         pc_validation_miss_decay_period));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++,
                         pc_validation_low_entry_miss_streak_threshold));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, pc_validation_epoch_bits));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++,
                         pc_validation_offset_context_slots));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++,
                         global_bop_unused_threshold));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++,
                         global_bop_min_resolved_coverage_shift));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, negative_offsets_enable));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, auto_learning));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, victim_offsets_list_size));
  bind(sqlite3_bind_int(bopReplayMetaStmt, column++, restore_cycle));
  bind(sqlite3_bind_int64(bopReplayMetaStmt, column++,
                          sqliteSignedInt(clock_period_ticks)));
  bind(sqlite3_bind_text(bopReplayMetaStmt, column++, offsets.c_str(), -1,
                         SQLITE_TRANSIENT));
  stepAndReset(mem_db, bopReplayMetaStmt, "BOPReplayMeta");
}

void
ArchDBer::bopReplayPhaseTraceWrite(
    uint64_t phase_id, const char *phase_name, Tick start_tick)
{
  if (!dumpBopReplayTrace) {
    return;
  }

  int column = 1;
  const auto bind = [this](int result) {
    fatal_if(result != SQLITE_OK, "Failed to bind BOPReplayPhase: %s\n",
             sqlite3_errmsg(mem_db));
  };
  bind(sqlite3_bind_int64(bopReplayPhaseStmt, column++,
                          sqliteSignedInt(phase_id)));
  bind(sqlite3_bind_text(bopReplayPhaseStmt, column++, phase_name, -1,
                         SQLITE_TRANSIENT));
  bind(sqlite3_bind_int64(bopReplayPhaseStmt, column++,
                          sqliteSignedInt(start_tick)));
  stepAndReset(mem_db, bopReplayPhaseStmt, "BOPReplayPhase");
}

void
ArchDBer::bopReplayDemandTraceWrite(
    uint64_t access_seq, Tick tick, Addr addr, Addr pc, bool has_pc,
    bool cache_miss, int prefetch_source, bool pf_first_hit, bool pf_hit)
{
  if (!(dumpGlobal && dumpBopReplayTrace)) {
    return;
  }

  int column = 1;
  const auto bind = [this](int result) {
    fatal_if(result != SQLITE_OK, "Failed to bind L2DemandTrace: %s\n",
             sqlite3_errmsg(mem_db));
  };
  bind(sqlite3_bind_int64(bopReplayDemandStmt, column++,
                          sqliteSignedInt(access_seq)));
  bind(sqlite3_bind_int64(bopReplayDemandStmt, column++,
                          sqliteSignedInt(bopReplayPhaseId)));
  bind(sqlite3_bind_int64(bopReplayDemandStmt, column++,
                          sqliteSignedInt(tick)));
  bind(sqlite3_bind_int64(bopReplayDemandStmt, column++,
                          sqliteSignedInt(addr)));
  bind(sqlite3_bind_int64(bopReplayDemandStmt, column++, sqliteSignedInt(pc)));
  bind(sqlite3_bind_int(bopReplayDemandStmt, column++, has_pc));
  bind(sqlite3_bind_int(bopReplayDemandStmt, column++, cache_miss));
  bind(sqlite3_bind_int(bopReplayDemandStmt, column++, prefetch_source));
  bind(sqlite3_bind_int(bopReplayDemandStmt, column++, pf_first_hit));
  bind(sqlite3_bind_int(bopReplayDemandStmt, column++, pf_hit));
  stepAndReset(mem_db, bopReplayDemandStmt, "L2DemandTrace");
}

void
ArchDBer::bopReplayEventTraceWrite(
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
    bool online_filter_passed)
{
  if (!(dumpGlobal && dumpBopReplayTrace)) {
    return;
  }

  int column = 1;
  const auto bind = [this](int result) {
    fatal_if(result != SQLITE_OK, "Failed to bind BOPReplayEvent: %s\n",
             sqlite3_errmsg(mem_db));
  };
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                          sqliteSignedInt(access_seq)));
  bind(sqlite3_bind_text(bopReplayEventStmt, column++, bop_name, -1,
                         SQLITE_TRANSIENT));
  bind(sqlite3_bind_text(bopReplayEventStmt, column++, bop_kind, -1,
                         SQLITE_TRANSIENT));
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                          sqliteSignedInt(replay_order)));
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                          sqliteSignedInt(bopReplayPhaseId)));
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                          sqliteSignedInt(tick)));
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                          sqliteSignedInt(trigger_addr)));
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                          sqliteSignedInt(trigger_pc)));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, trigger_has_pc));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, trigger_is_demand));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, trigger_is_read));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, trigger_cache_miss));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, trigger_pf_source));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, trigger_pf_first_hit));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, trigger_pf_hit));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, late));
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                          sqliteSignedInt(best_offset_before)));
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                          sqliteSignedInt(best_offset_after)));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, best_score));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, round));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, best_offset_changed));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, issue_enabled));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, validation_enabled));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, validation_hit));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++,
                         pc_confidence_enabled));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, pc_index));
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                          sqliteSignedInt(pc_tag)));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, pc_entry_hit));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, pc_confidence));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, pc_state));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, pc_sampled));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++,
                         pc_low_entry_miss_streak));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, pc_epoch));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, global_bypass_active));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, policy_suppressed));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, raw_candidate_valid));
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                          sqliteSignedInt(raw_candidate_addr)));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++,
                         policy_candidate_valid));
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                         sqliteSignedInt(policy_candidate_addr)));
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                          sqliteSignedInt(validation_addr)));
  bind(sqlite3_bind_int64(bopReplayEventStmt, column++,
                          sqliteSignedInt(prefetch_addr)));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, online_generated));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, online_buffered));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, online_filtered));
  bind(sqlite3_bind_int(bopReplayEventStmt, column++, online_filter_passed));
  stepAndReset(mem_db, bopReplayEventStmt, "BOPReplayEvent");
}

void
ArchDBer::bopReplayDelayActionTraceWrite(
    const char *bop_name, uint64_t replay_order, const char *action,
    Tick tick, Addr addr, Tick process_tick, unsigned int queue_size_after)
{
  if (!(dumpGlobal && dumpBopReplayTrace)) {
    return;
  }

  int column = 1;
  const auto bind = [this](int result) {
    fatal_if(result != SQLITE_OK,
             "Failed to bind BOPReplayDelayAction: %s\n", sqlite3_errmsg(mem_db));
  };
  bind(sqlite3_bind_text(bopReplayDelayActionStmt, column++, bop_name, -1,
                         SQLITE_TRANSIENT));
  bind(sqlite3_bind_int64(bopReplayDelayActionStmt, column++,
                          sqliteSignedInt(replay_order)));
  bind(sqlite3_bind_text(bopReplayDelayActionStmt, column++, action, -1,
                         SQLITE_TRANSIENT));
  bind(sqlite3_bind_int64(bopReplayDelayActionStmt, column++,
                          sqliteSignedInt(tick)));
  bind(sqlite3_bind_int64(bopReplayDelayActionStmt, column++,
                          sqliteSignedInt(addr)));
  bind(sqlite3_bind_int64(bopReplayDelayActionStmt, column++,
                          sqliteSignedInt(process_tick)));
  bind(sqlite3_bind_int(bopReplayDelayActionStmt, column++, queue_size_after));
  stepAndReset(mem_db, bopReplayDelayActionStmt, "BOPReplayDelayAction");
}

void
ArchDBer::bopDirectQualityMetaTraceWrite(
    const prefetch::DirectQualityGate::Config &config)
{
  if (!(dumpGlobal && dumpBopDirectQualityTrace)) {
    return;
  }

  int column = 1;
  const auto bind = [this](int result) {
    fatal_if(result != SQLITE_OK,
             "Failed to bind BOPDirectQualityMeta: %s\n", sqlite3_errmsg(mem_db));
  };
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.qualityEntries));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.qualityWays));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.qualityTagBits));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.feedbackEntries));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.feedbackWays));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.horizon));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.minSamples));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.observeSamplePeriod));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.openSamplePeriod));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.blockProbePeriod));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.borderlineBlockProbePeriod));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.unusedPerUseful));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.blockGuard));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.strictUnusedPerUseful));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.strictBlockGuard));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.reopenUnusedPerUseful));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.reopenGuard));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.reopenProbePeriod));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.reopenConfirmSamples));
  bind(sqlite3_bind_int(bopDirectQualityMetaStmt, column++, config.decayPeriod));
  stepAndReset(mem_db, bopDirectQualityMetaStmt, "BOPDirectQualityMeta");
}

void
ArchDBer::bopDirectQualityCandidateTraceWrite(
    Tick tick, uint64_t event_sequence, Addr pc, uint8_t kind,
    Addr trigger_line, Addr candidate_line, uint8_t state, bool allowed,
    bool sampled)
{
  if (!(dumpGlobal && dumpBopDirectQualityTrace)) {
    return;
  }

  int column = 1;
  const auto bind = [this](int result) {
    fatal_if(result != SQLITE_OK,
             "Failed to bind BOPDirectQualityCandidate: %s\n",
             sqlite3_errmsg(mem_db));
  };
  bind(sqlite3_bind_int64(bopDirectQualityCandidateStmt, column++,
                          sqliteSignedInt(event_sequence)));
  bind(sqlite3_bind_int64(bopDirectQualityCandidateStmt, column++,
                          sqliteSignedInt(tick)));
  bind(sqlite3_bind_int64(bopDirectQualityCandidateStmt, column++,
                          sqliteSignedInt(pc)));
  bind(sqlite3_bind_int(bopDirectQualityCandidateStmt, column++, kind));
  bind(sqlite3_bind_int64(bopDirectQualityCandidateStmt, column++,
                          sqliteSignedInt(trigger_line)));
  bind(sqlite3_bind_int64(bopDirectQualityCandidateStmt, column++,
                          sqliteSignedInt(candidate_line)));
  bind(sqlite3_bind_int(bopDirectQualityCandidateStmt, column++, state));
  bind(sqlite3_bind_int(bopDirectQualityCandidateStmt, column++, allowed));
  bind(sqlite3_bind_int(bopDirectQualityCandidateStmt, column++, sampled));
  stepAndReset(mem_db, bopDirectQualityCandidateStmt,
               "BOPDirectQualityCandidate");
}

void
ArchDBer::bopDirectQualityIssueTraceWrite(
    Tick tick, uint64_t event_sequence, uint64_t feedback_id,
    uint64_t candidate_demand_sequence, Addr line, uint8_t kind)
{
  if (!(dumpGlobal && dumpBopDirectQualityTrace)) {
    return;
  }

  int column = 1;
  const auto bind = [this](int result) {
    fatal_if(result != SQLITE_OK,
             "Failed to bind BOPDirectQualityIssue: %s\n", sqlite3_errmsg(mem_db));
  };
  bind(sqlite3_bind_int64(bopDirectQualityIssueStmt, column++,
                          sqliteSignedInt(event_sequence)));
  bind(sqlite3_bind_int64(bopDirectQualityIssueStmt, column++,
                          sqliteSignedInt(feedback_id)));
  bind(sqlite3_bind_int64(bopDirectQualityIssueStmt, column++,
                          sqliteSignedInt(candidate_demand_sequence)));
  bind(sqlite3_bind_int64(bopDirectQualityIssueStmt, column++,
                          sqliteSignedInt(tick)));
  bind(sqlite3_bind_int64(bopDirectQualityIssueStmt, column++,
                          sqliteSignedInt(line)));
  bind(sqlite3_bind_int(bopDirectQualityIssueStmt, column++, kind));
  stepAndReset(mem_db, bopDirectQualityIssueStmt, "BOPDirectQualityIssue");
}

void
ArchDBer::bopDirectQualityDemandTraceWrite(
    Tick tick, uint64_t event_sequence, uint64_t demand_sequence, Addr line)
{
  if (!(dumpGlobal && dumpBopDirectQualityTrace)) {
    return;
  }

  int column = 1;
  const auto bind = [this](int result) {
    fatal_if(result != SQLITE_OK,
             "Failed to bind BOPDirectQualityDemand: %s\n", sqlite3_errmsg(mem_db));
  };
  bind(sqlite3_bind_int64(bopDirectQualityDemandStmt, column++,
                          sqliteSignedInt(event_sequence)));
  bind(sqlite3_bind_int64(bopDirectQualityDemandStmt, column++,
                          sqliteSignedInt(demand_sequence)));
  bind(sqlite3_bind_int64(bopDirectQualityDemandStmt, column++,
                          sqliteSignedInt(tick)));
  bind(sqlite3_bind_int64(bopDirectQualityDemandStmt, column++,
                          sqliteSignedInt(line)));
  stepAndReset(mem_db, bopDirectQualityDemandStmt, "BOPDirectQualityDemand");
}

void
ArchDBer::bopDirectQualityOutcomeTraceWrite(
    Tick tick, uint64_t event_sequence, uint64_t feedback_id,
    uint64_t resolve_demand_sequence, Addr line, const char *outcome)
{
  if (!(dumpGlobal && dumpBopDirectQualityTrace)) {
    return;
  }

  int column = 1;
  const auto bind = [this](int result) {
    fatal_if(result != SQLITE_OK,
             "Failed to bind BOPDirectQualityOutcome: %s\n",
             sqlite3_errmsg(mem_db));
  };
  bind(sqlite3_bind_int64(bopDirectQualityOutcomeStmt, column++,
                          sqliteSignedInt(event_sequence)));
  bind(sqlite3_bind_int64(bopDirectQualityOutcomeStmt, column++,
                          sqliteSignedInt(feedback_id)));
  bind(sqlite3_bind_int64(bopDirectQualityOutcomeStmt, column++,
                          sqliteSignedInt(resolve_demand_sequence)));
  bind(sqlite3_bind_int64(bopDirectQualityOutcomeStmt, column++,
                          sqliteSignedInt(tick)));
  bind(sqlite3_bind_int64(bopDirectQualityOutcomeStmt, column++,
                          sqliteSignedInt(line)));
  bind(sqlite3_bind_text(bopDirectQualityOutcomeStmt, column++, outcome, -1,
                         SQLITE_TRANSIENT));
  stepAndReset(mem_db, bopDirectQualityOutcomeStmt,
               "BOPDirectQualityOutcome");
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
