//Created on 24-01-03
//choose stride or berti in sms

#include "mem/cache/prefetch/xs_stride.hh"

#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>

#include "base/output.hh"
#include "base/stats/group.hh"
#include "debug/XSStridePrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace prefetch
{

namespace
{

bool
tableExists(sqlite3 *db, const std::string &table)
{
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_TRANSIENT);
    const bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

long long
sqliteSignedInt(uint64_t value)
{
    return static_cast<long long>(static_cast<int64_t>(value));
}

std::string
sqlEscape(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        escaped += c;
        if (c == '\'') {
            escaped += '\'';
        }
    }
    return escaped;
}

} // anonymous namespace

XSStridePrefetcher::XSStridePrefetcher(const XSStridePrefetcherParams &p)
    : Queued(p),useXsDepth(p.use_xs_depth),useRedundantTable(p.use_redundant_table),
      fuzzyStrideMatching(p.fuzzy_stride_matching),
      shortStrideThres(p.short_stride_thres),
      strideDynDepth(p.stride_dyn_depth),
      enableNonStrideFilter(p.enable_non_stride_filter),
      enableTraceDb(p.enable_trace_db),
      traceHartId(p.trace_hart_id),
      regionSize(p.region_size),
      regionBlks(p.region_size / p.block_size),
      traceDbFile(p.trace_db_file),
      strideUnique(p.stride_entries, p.stride_entries, p.stride_unique_indexing_policy,
             p.stride_unique_replacement_policy, StrideEntry()),
      strideRedundant(p.stride_entries, p.stride_entries, p.stride_redundant_indexing_policy,
             p.stride_redundant_replacement_policy, StrideEntry()),
      nonStridePCs(p.non_stride_assoc, p.non_stride_entries, p.non_stride_indexing_policy,
             p.non_stride_replacement_policy, NonStrideEntry()),
      commitTrainEvent([this]{ processCommitTrain(); }, name()),
      filter(nullptr),
      filterL2(nullptr),
      stridestream_pfFilter_l1(nullptr),
      stridestream_pfFilter_l2l3(nullptr),
      stats(this)
{
    if (enableTraceDb) {
        initReplayTraceDb(p);
    }
}

XSStridePrefetcher::~XSStridePrefetcher()
{
    if (ownTraceDb && traceDb) {
        sqlite3_close(traceDb);
        traceDb = nullptr;
    }
}

void
XSStridePrefetcher::initReplayTraceDb(const XSStridePrefetcherParams &p)
{
    const bool useArchDb = (archDBer != nullptr) && (archDBer->mem_db != nullptr);
    if (useArchDb) {
        traceDb = archDBer->mem_db;
    } else {
        int rc = sqlite3_open(":memory:", &traceDb);
        if (rc != SQLITE_OK || !traceDb) {
            fatal("Can't open XSStride trace sqlite database\n");
        }
        ownTraceDb = true;

        if (traceDbFile.empty()) {
            traceDbFile = "sstride_trace_h" + std::to_string(traceHartId) + ".db";
        }

        registerExitCallback([this]() { saveReplayTraceDb(); });
    }

    const std::string suffix = "_h" + std::to_string(traceHartId);
    replayConfigTableName = "SStrideConfigTrace" + suffix;
    replayInputTableName = "SStrideInputTrace" + suffix;
    replayCandidateTableName = "SStrideCandidateTrace" + suffix;

    if (!tableExists(traceDb, replayConfigTableName)) {
        execReplayTraceSql(
            "CREATE TABLE " + replayConfigTableName +
            "(TRACEKIND TEXT NOT NULL,"
            "SITE TEXT NOT NULL,"
            "BLOCKSIZE INT NOT NULL,"
            "USEXSDEPTH INT NOT NULL,"
            "USEREDUNDANTTABLE INT NOT NULL,"
            "FUZZYSTRIDEMATCHING INT NOT NULL,"
            "SHORTSTRIDETHRES INT NOT NULL,"
            "STRIDEDYNDEPTH INT NOT NULL,"
            "ENABLENONSTRIDEFILTER INT NOT NULL,"
            "STRIDEENTRIES INT NOT NULL,"
            "NONSTRIDEENTRIES INT NOT NULL,"
            "NONSTRIDEASSOC INT NOT NULL,"
            "USEVADDR INT NOT NULL,"
            "SEMANTICS TEXT NOT NULL,"
            "PRIMARY KEY (TRACEKIND, SITE));");
    }

    if (!tableExists(traceDb, replayInputTableName)) {
        execReplayTraceSql(
            "CREATE TABLE " + replayInputTableName +
            "(ID INTEGER PRIMARY KEY AUTOINCREMENT,"
            "ADDR INT NOT NULL,"
            "PC INT NOT NULL,"
            "SECURE INT NOT NULL,"
            "CACHEMISS INT NOT NULL,"
            "LATE INT NOT NULL,"
            "PFSOURCE INT NOT NULL,"
            "MISSREPEAT INT NOT NULL,"
            "ENTERNEWREGION INT NOT NULL,"
            "FIRSTSHOT INT NOT NULL,"
            "STAMP INT NOT NULL,"
            "SITE TEXT);");
    }

    if (!tableExists(traceDb, replayCandidateTableName)) {
        execReplayTraceSql(
            "CREATE TABLE " + replayCandidateTableName +
            "(ID INTEGER PRIMARY KEY AUTOINCREMENT,"
            "INPUTID INT NOT NULL,"
            "TRIGGERADDR INT NOT NULL,"
            "TRIGGERPC INT NOT NULL,"
            "PREFETCHADDR INT NOT NULL,"
            "PRIORITY INT NOT NULL,"
            "PFAHEAD INT NOT NULL,"
            "PFAHEADHOST INT NOT NULL,"
            "AHEADLEVEL INT NOT NULL,"
            "STAMP INT NOT NULL,"
            "SITE TEXT);");
    }

    recordReplayConfigTrace(p);
}

void
XSStridePrefetcher::saveReplayTraceDb() const
{
    if (!ownTraceDb || !traceDb) {
        return;
    }

    const auto path = simout.resolve(traceDbFile);
    warn("saving XSStride trace db to %s ...\n", path.c_str());
    sqlite3 *diskDb = nullptr;
    sqlite3_backup *backup = nullptr;
    int rc = sqlite3_open(path.c_str(), &diskDb);
    if (rc == SQLITE_OK) {
        backup = sqlite3_backup_init(diskDb, "main", traceDb, "main");
        if (backup) {
            (void)sqlite3_backup_step(backup, -1);
            (void)sqlite3_backup_finish(backup);
        }
        rc = sqlite3_errcode(diskDb);
    }
    fatal_if(rc != SQLITE_OK, "Can't save XSStride trace db: %s\n",
             diskDb ? sqlite3_errmsg(diskDb) : "sqlite open failed");
    sqlite3_close(diskDb);
}

void
XSStridePrefetcher::execReplayTraceSql(const std::string &sql) const
{
    if (!traceDb) {
        return;
    }

    char *errMsg = nullptr;
    const int rc = sqlite3_exec(traceDb, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        fatal("XSStride trace SQL error: %s\n", errMsg ? errMsg : "unknown");
    }
}

void
XSStridePrefetcher::recordReplayConfigTrace(const XSStridePrefetcherParams &p)
{
    if (!enableTraceDb || !traceDb) {
        return;
    }

    const std::string sql =
        "INSERT OR REPLACE INTO " + replayConfigTableName +
        "(TRACEKIND,SITE,BLOCKSIZE,USEXSDEPTH,USEREDUNDANTTABLE,"
        "FUZZYSTRIDEMATCHING,SHORTSTRIDETHRES,STRIDEDYNDEPTH,"
        "ENABLENONSTRIDEFILTER,STRIDEENTRIES,NONSTRIDEENTRIES,"
        "NONSTRIDEASSOC,USEVADDR,SEMANTICS) VALUES('SStride','" +
        sqlEscape(name()) + "'," +
        std::to_string(blkSize) + "," +
        std::to_string(useXsDepth ? 1 : 0) + "," +
        std::to_string(useRedundantTable ? 1 : 0) + "," +
        std::to_string(fuzzyStrideMatching ? 1 : 0) + "," +
        std::to_string(shortStrideThres) + "," +
        std::to_string(strideDynDepth ? 1 : 0) + "," +
        std::to_string(enableNonStrideFilter ? 1 : 0) + "," +
        std::to_string(static_cast<unsigned>(p.stride_entries)) + "," +
        std::to_string(static_cast<unsigned>(p.non_stride_entries)) + "," +
        std::to_string(static_cast<unsigned>(p.non_stride_assoc)) + "," +
        std::to_string(useVirtualAddresses ? 1 : 0) + "," +
        "'production');";
    execReplayTraceSql(sql);
}

void
XSStridePrefetcher::recordReplayInputTrace(const PrefetchInfo &pfi, bool late,
                                           PrefetchSourceType pf_source,
                                           bool miss_repeat,
                                           bool enter_new_region,
                                           bool is_first_shot)
{
    if (!enableTraceDb || !traceDb) {
        lastReplayInputId = 0;
        return;
    }

    const uint64_t stamp = curCycle();
    const std::string sql =
        "INSERT INTO " + replayInputTableName +
        "(ADDR,PC,SECURE,CACHEMISS,LATE,PFSOURCE,MISSREPEAT,"
        "ENTERNEWREGION,FIRSTSHOT,STAMP,SITE) VALUES(" +
        std::to_string(sqliteSignedInt(pfi.getAddr())) + "," +
        std::to_string(sqliteSignedInt(pfi.getPC())) + "," +
        std::to_string(pfi.isSecure() ? 1 : 0) + "," +
        std::to_string(pfi.isCacheMiss() ? 1 : 0) + "," +
        std::to_string(late ? 1 : 0) + "," +
        std::to_string(static_cast<unsigned>(pf_source)) + "," +
        std::to_string(miss_repeat ? 1 : 0) + "," +
        std::to_string(enter_new_region ? 1 : 0) + "," +
        std::to_string(is_first_shot ? 1 : 0) + "," +
        std::to_string(sqliteSignedInt(stamp)) + ",'" + sqlEscape(name()) + "');";
    execReplayTraceSql(sql);
    lastReplayInputId = static_cast<uint64_t>(sqlite3_last_insert_rowid(traceDb));
}

void
XSStridePrefetcher::recordReplayCandidateTrace(Addr trigger_addr, Addr trigger_pc,
                                               Addr pf_addr, int priority,
                                               bool pfahead, int pfahead_host,
                                               int ahead_level)
{
    if (!enableTraceDb || !traceDb) {
        return;
    }

    const uint64_t stamp = curCycle() + Cycles(1);
    const std::string sql =
        "INSERT INTO " + replayCandidateTableName +
        "(INPUTID,TRIGGERADDR,TRIGGERPC,PREFETCHADDR,PRIORITY,PFAHEAD,"
        "PFAHEADHOST,AHEADLEVEL,STAMP,SITE) VALUES(" +
        std::to_string(sqliteSignedInt(lastReplayInputId)) + "," +
        std::to_string(sqliteSignedInt(trigger_addr)) + "," +
        std::to_string(sqliteSignedInt(trigger_pc)) + "," +
        std::to_string(sqliteSignedInt(pf_addr)) + "," +
        std::to_string(priority) + "," +
        std::to_string(pfahead ? 1 : 0) + "," +
        std::to_string(pfahead_host) + "," +
        std::to_string(ahead_level) + "," +
        std::to_string(sqliteSignedInt(stamp)) + ",'" + sqlEscape(name()) + "');";
    execReplayTraceSql(sql);
}

void
XSStridePrefetcher::traceCommitOrderStage(const char *stage,
                                          const CommitTrainSnapshot &snapshot,
                                          int queue_size,
                                          const char *reason) const
{
    traceCommitOrderStage(stage, snapshot.seqNum, snapshot.pc, snapshot.addr,
                          true, queue_size, reason);
}

void
XSStridePrefetcher::traceCommitOrderStage(const char *stage,
                                          InstSeqNum seq_num,
                                          Addr pc,
                                          Addr addr,
                                          bool is_load,
                                          int queue_size,
                                          const char *reason) const
{
    if (!archDBer) {
        return;
    }

    archDBer->strideOrderTraceWrite(
        curTick(), stage, seq_num, pc, addr, blockAddress(addr), is_load,
        false, PrefetchSourceType::SStride, 0, curTick(), queue_size, reason);
}

void
XSStridePrefetcher::scheduleCommitTrain()
{
    if (readyToTrain.empty() || commitTrainEvent.scheduled()) {
        return;
    }

    schedule(commitTrainEvent, clockEdge(Cycles(1)) + 1);
}

void
XSStridePrefetcher::triggerFromCommitTable(const PrefetchInfo &pfi,
                                           std::vector<AddrPriority> &addresses)
{
    if (!pfi.hasPC()) {
        return;
    }

    stats.strideUniquequeryCount++;

    if (enableNonStrideFilter && isNonStridePC(pfi.getPC())) {
        return;
    }

    const Addr lookupAddr = pfi.getAddr();
    const Addr stride_hash_pc = strideHashPc(pfi.getPC());
    const uint64_t triggerSeqNum = pfi.getSeqNum();
    StrideEntry *entry = strideUnique.findEntry(stride_hash_pc, pfi.isSecure());

    if (archDBer) {
        archDBer->strideTraceWrite(curTick(), lookupAddr, pfi.getPC(),
                                   stride_hash_pc, entry != nullptr, true,
                                   false, false, triggerSeqNum);
    }

    if (!entry) {
        stats.strideUniquemissCount++;
        return;
    }

    stats.strideUniquehitCount++;
    strideUnique.accessEntry(entry);

    if (entry->conf < 2 || entry->stride == 0) {
        return;
    }

    if (useXsDepth) {
        sendPFWithFilter(pfi, blockAddress(lookupAddr + (entry->stride << 4)),
                         addresses, 0, PrefetchSourceType::SStride, 1);
        sendPFWithFilter(pfi, blockAddress(lookupAddr + (entry->stride << 7)),
                         addresses, 0, PrefetchSourceType::SStride, 2);
        stats.strideUniquepfCount += 2;
        if (archDBer) {
            archDBer->strideTraceWrite(
                curTick(), blockAddress(lookupAddr + (entry->stride << 2)),
                pfi.getPC(), stride_hash_pc, true, true, false, false,
                triggerSeqNum);
            archDBer->strideTraceWrite(
                curTick(), blockAddress(lookupAddr + (entry->stride << 5)),
                pfi.getPC(), stride_hash_pc, true, true, false, false,
                triggerSeqNum);
        }
        return;
    }

    const unsigned depth = std::max(1, entry->depth);
    const Addr pf_addr = lookupAddr + entry->stride * depth;
    sendPFWithFilter(pfi, blockAddress(pf_addr), addresses, 0,
                     PrefetchSourceType::SStride, 1);
    stats.strideUniquepfCount++;
}

void
XSStridePrefetcher::captureAndTriggerFromS1(const PrefetchInfo &pfi,
                                            std::vector<AddrPriority> &addresses)
{
    if (!pfi.hasPC() || !pfi.hasSeqNum()) {
        return;
    }

    const InstSeqNum seq_num = pfi.getSeqNum();
    auto [it, inserted] = pendingSnapshots.try_emplace(seq_num, pfi);
    if (!inserted) {
        stats.commitOrderedS1DuplicateCount++;
        traceCommitOrderStage("SkipDup", seq_num, pfi.getPC(), pfi.getAddr(),
                              true, pendingSnapshots.size(), "duplicate_seq");
        return;
    }

    stats.commitOrderedS1CaptureCount++;
    traceCommitOrderStage("S1Capture", it->second, pendingSnapshots.size(), "");
    triggerFromCommitTable(pfi, addresses);
    traceCommitOrderStage("S1Trigger", it->second, pendingSnapshots.size(), "");
}

void
XSStridePrefetcher::markCommitted(InstSeqNum seq_num)
{
    auto it = pendingSnapshots.find(seq_num);
    if (it == pendingSnapshots.end()) {
        return;
    }

    if (it->second.readyForTrain) {
        return;
    }

    it->second.readyForTrain = true;
    it->second.readyCycle = curCycle();
    readyToTrain.insert(seq_num);
    stats.commitOrderedReadyCount++;
    traceCommitOrderStage("CommitReady", it->second, readyToTrain.size(), "");
    scheduleCommitTrain();
}

void
XSStridePrefetcher::dropYoungerThan(InstSeqNum boundary)
{
    readyToTrain.erase(readyToTrain.upper_bound(boundary), readyToTrain.end());

    for (auto it = pendingSnapshots.upper_bound(boundary);
         it != pendingSnapshots.end(); ) {
        stats.commitOrderedSquashDropCount++;
        traceCommitOrderStage("DropSquash", it->second, pendingSnapshots.size(),
                              "squash_boundary");
        it = pendingSnapshots.erase(it);
    }
}

void
XSStridePrefetcher::trainFromSnapshot(const CommitTrainSnapshot &snapshot)
{
    stats.commitOrderedTrainEnterCount++;

    if (enableNonStrideFilter && isNonStridePC(snapshot.pc)) {
        stats.commitOrderedTrainFilteredNonStrideCount++;
        return;
    }

    const Addr lookupAddr = snapshot.addr;
    const Addr stride_hash_pc = strideHashPc(snapshot.pc);
    StrideEntry *entry = strideUnique.findEntry(stride_hash_pc, snapshot.secure);

    if (archDBer) {
        archDBer->strideTraceWrite(curTick(), lookupAddr, snapshot.pc,
                                   stride_hash_pc, entry != nullptr, true,
                                   false, true, snapshot.seqNum);
    }

    if (entry) {
        strideUnique.accessEntry(entry);
        const int64_t new_stride = lookupAddr - entry->lastAddr;
        if (new_stride == 0) {
            stats.commitOrderedTrainZeroStrideCount++;
            return;
        }
        if (labs(new_stride) < 64 &&
            entry->longStride.calcSaturation() >= 0.5) {
            stats.commitOrderedTrainGuardedCount++;
            return;
        }

        bool stride_match =
            fuzzyStrideMatching &&
            entry->stride > 64 &&
            entry->stride != 0 &&
            new_stride % entry->stride == 0;
        stride_match |= new_stride == entry->stride;

        if (shortStrideThres) {
            stats.commitOrderedTrainLongStrideAdjustCount++;
            if (labs(new_stride) > shortStrideThres) {
                entry->longStride.saturate();
            } else {
                entry->longStride--;
            }
        }

        if (shortStrideThres &&
            entry->longStride.calcSaturation() > 0.5 &&
            labs(new_stride) < shortStrideThres) {
            stats.commitOrderedTrainGuardedCount++;
            return;
        }

        if (stride_match) {
            stats.commitOrderedTrainUpdateCount++;
            stats.commitOrderedTrainMatchCount++;
            entry->conf++;
            entry->lastAddr = lookupAddr;
            entry->histStrides.clear();
            entry->matchedSinceAlloc = true;
        } else if (labs(entry->stride) > 64L && labs(new_stride) < 64L) {
            stats.commitOrderedTrainGuardedCount++;
            return;
        } else {
            stats.commitOrderedTrainUpdateCount++;
            stats.commitOrderedTrainMismatchCount++;
            entry->conf--;
            entry->lastAddr = lookupAddr;
            if ((int)entry->conf == 0) {
                bool found_in_hist = false;
                if (enableNonStrideFilter) {
                    if (entry->stride != 0) {
                        entry->histStrides.push_back(entry->stride);
                    }
                    for (auto it = entry->histStrides.begin();
                         it != entry->histStrides.end(); ++it) {
                        if (*it == new_stride) {
                            found_in_hist = true;
                            entry->histStrides.erase(it);
                            break;
                        }
                    }
                    if (found_in_hist) {
                        entry->histStrides.clear();
                    }
                }

                if (enableNonStrideFilter && !found_in_hist &&
                    entry->histStrides.size() >= maxHistStrides) {
                    markNonStridePC(entry->pc);
                    entry->histStrides.clear();
                    entry->invalidate();
                    return;
                }

                entry->stride = new_stride;
                entry->depth = 1;
                entry->lateConf.reset();
                stats.commitOrderedTrainRetargetCount++;
            }
        }

        periodStrideDepthDown();
        return;
    }

    stats.commitOrderedTrainUpdateCount++;
    stats.commitOrderedTrainAllocCount++;
    entry = strideUnique.findVictim(0);
    if (enableNonStrideFilter &&
        (entry->histStrides.size() >= maxHistStrides - 1 ||
         !entry->matchedSinceAlloc)) {
        markNonStridePC(entry->pc);
    }
    if (entry->conf >= 2) {
        stats.strideUniquereplaceusefulCount++;
    }

    entry->conf.reset();
    entry->lastAddr = lookupAddr;
    entry->stride = 0;
    entry->depth = 1;
    entry->lateConf.reset();
    entry->pc = snapshot.pc;
    entry->histStrides.clear();
    entry->matchedSinceAlloc = false;
    strideUnique.insertEntry(stride_hash_pc, snapshot.secure, entry);

    periodStrideDepthDown();
}

void
XSStridePrefetcher::processCommitTrain()
{
    const Cycles current_cycle = curCycle();

    while (!readyToTrain.empty()) {
        const auto ready_it = readyToTrain.begin();
        const InstSeqNum seq_num = *ready_it;
        auto it = pendingSnapshots.find(seq_num);
        if (it == pendingSnapshots.end()) {
            readyToTrain.erase(ready_it);
            continue;
        }

        if (it->second.readyCycle >= current_cycle) {
            stats.commitOrderedDeferCount++;
            traceCommitOrderStage("CommitDefer", it->second,
                                  readyToTrain.size(), "ready_this_cycle");
            break;
        }

        const CommitTrainSnapshot snapshot = it->second;
        readyToTrain.erase(ready_it);
        pendingSnapshots.erase(it);
        stats.commitOrderedTrainDispatchCount++;
        traceCommitOrderStage("CommitTrain", snapshot, readyToTrain.size(),
                              "");
        trainFromSnapshot(snapshot);
        break;
    }

    if (!readyToTrain.empty()) {
        scheduleCommitTrain();
    }
}

void
XSStridePrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                                       PrefetchSourceType pf_source, bool miss_repeat, bool enter_new_region,
                                       bool is_first_shot, Addr &pf_addr, int64_t &learned_bop_offset)
{
    recordReplayInputTrace(pfi, late, pf_source, miss_repeat, enter_new_region,
                           is_first_shot);
    if (is_first_shot ||!useRedundantTable) {
        DPRINTF(XSStridePrefetcher, "Do stride lookup for first shot acc ...\n");
        strideLookup(strideUnique, pfi, addresses, late, pf_addr, pf_source, enter_new_region, miss_repeat,
                     learned_bop_offset, is_first_shot);
    } else {
        DPRINTF(XSStridePrefetcher, "Do stride lookup for repeat acc ...\n");
        strideLookup(strideRedundant, pfi, addresses, late, pf_addr, pf_source, enter_new_region, miss_repeat,
                     learned_bop_offset, is_first_shot);
    }
}
bool
XSStridePrefetcher::strideLookup(AssociativeSet<StrideEntry> &stride, const PrefetchInfo &pfi,
                                  std::vector<AddrPriority> &addresses, bool late, Addr &stride_pf,
                                  PrefetchSourceType last_pf_source, bool enter_new_region, bool miss_repeat,
                                  int64_t &learned_bop_offset, bool is_first_shot)
{
    if (is_first_shot) {
        stats.strideUniquequeryCount++;
    } else {
        stats.strideRedundantqueryCount++;
    }
    Addr lookupAddr = pfi.getAddr();
    Addr stride_hash_pc = strideHashPc(pfi.getPC());
    const uint64_t triggerSeqNum = pfi.getSeqNum();
    StrideEntry *entry = stride.findEntry(stride_hash_pc, pfi.isSecure());
    learned_bop_offset = 0;
    // TODO: add DPRINFT for stride
    DPRINTF(XSStridePrefetcher, "Stride lookup: pc:%x addr: %x, miss repeat: %i\n", pfi.getPC(), lookupAddr,
            miss_repeat);
    bool should_cover = false;
    if (entry) {
        if (archDBer){
            archDBer->strideTraceWrite(curTick(), lookupAddr, pfi.getPC(), stride_hash_pc,
                                       true, is_first_shot, pfi.isCacheMiss(), true,
                                       triggerSeqNum);
        }
    }else{
        if (archDBer){
            archDBer->strideTraceWrite(curTick(), lookupAddr, pfi.getPC(), stride_hash_pc,
                                       false, is_first_shot, pfi.isCacheMiss(), true,
                                       triggerSeqNum);
        }
    }
    if (entry) {
        if (is_first_shot) {
            stats.strideUniquehitCount++;
        } else {
            stats.strideRedundanthitCount++;
        }
        stride.accessEntry(entry);
        int64_t new_stride = lookupAddr - entry->lastAddr;
        if (new_stride == 0 || (labs(new_stride) < 64 && (miss_repeat || entry->longStride.calcSaturation() >= 0.5))) {
            DPRINTF(XSStridePrefetcher, "Stride touch in the same blk, ignore redundant req\n");
            return false;
        }
        bool stride_match = fuzzyStrideMatching ? (entry->stride > 64 && new_stride % entry->stride == 0) : false;
        stride_match |= new_stride == entry->stride;
        DPRINTF(XSStridePrefetcher, "Stride hit, with stride: %ld(%lx), old stride: %ld(%lx), long stride: %.2f\n",
                new_stride, new_stride, entry->stride, entry->stride, entry->longStride.calcSaturation());

        if (shortStrideThres) {
            if (labs(new_stride) > shortStrideThres) {
                entry->longStride.saturate();
            } else {
                entry->longStride--;
            }
        }

        if (shortStrideThres && entry->longStride.calcSaturation() > 0.5 && labs(new_stride) < shortStrideThres) {
            DPRINTF(XSStridePrefetcher, "Ignore short stride %li for long stride pattern\n", new_stride);
            return false;
        } else {
            DPRINTF(XSStridePrefetcher, "Stride long stride pattern: %.2f, short thres: %lu\n",
                    entry->longStride.calcSaturation(), shortStrideThres);
        }

        if (stride_match) {
            entry->conf++;
            if (strideDynDepth) {
                if (!pfi.isCacheMiss() && last_pf_source == PrefetchSourceType::SStride) {  // stride pref hit
                    entry->lateConf--;
                } else if (late) {  // stride pf late or other prefetcher late
                    entry->lateConf += 3;
                }
                if (entry->lateConf.isSaturated()) {
                    entry->depth++;
                    entry->lateConf.reset();
                } else if ((uint8_t)entry->lateConf == 0) {
                    entry->depth = std::max(1, entry->depth - 1);
                    entry->lateConf.reset();
                }
            }
            DPRINTF(XSStridePrefetcher, "Stride match, inc conf to %d, late: %i, late sat:%i, depth: %i\n",
                    (int)entry->conf, late, (uint8_t)entry->lateConf, entry->depth);
            entry->lastAddr = lookupAddr;
            entry->histStrides.clear();
            entry->matchedSinceAlloc = true;

        } else if (labs(entry->stride) > 64L && labs(new_stride) < 64L) {
            // different stride, but in the same cache line
            DPRINTF(XSStridePrefetcher, "Stride unmatch, but access goes to the same line, ignore\n");

        } else {
            entry->conf--;
            entry->lastAddr = lookupAddr;
            DPRINTF(XSStridePrefetcher, "Stride unmatch, dec conf to %d\n", (int)entry->conf);
            if ((int)entry->conf == 0) {
                DPRINTF(XSStridePrefetcher, "Stride conf = 0, reset stride to %ld\n", new_stride);

                bool found_in_hist = false;

                if (enableNonStrideFilter) {
                    if (entry->stride != 0) {
                        entry->histStrides.push_back(entry->stride);
                    }
                    for (auto it = entry->histStrides.begin(); it != entry->histStrides.end(); it++) {
                        DPRINTF(XSStridePrefetcher, "Stride hist: %ld, match: %i\n", *it, *it == new_stride);
                        if (*it == new_stride) {
                            found_in_hist = true;
                            entry->histStrides.erase(it);
                            break;
                        }
                    }
                    if (found_in_hist) {
                        entry->histStrides.clear();
                    }
                }

                if (enableNonStrideFilter && !found_in_hist && entry->histStrides.size() >= maxHistStrides) {
                    markNonStridePC(entry->pc);
                    entry->histStrides.clear();
                    entry->invalidate();
                } else {
                    entry->stride = new_stride;
                    entry->depth = 1;
                    entry->lateConf.reset();
                }
            }
        }
        if (entry->conf >= 2) {
            // if miss send 1*stride ~ depth*stride, else send depth*stride
            unsigned start_depth = pfi.isCacheMiss() ? std::max(1, (entry->depth - 4)) : entry->depth;
            Addr pf_addr = 0;
            if (useXsDepth) {
                sendPFWithFilter(pfi, blockAddress(lookupAddr + (entry->stride << 2)), addresses, 0,
                                 PrefetchSourceType::SStride, 1);
                sendPFWithFilter(pfi, blockAddress(lookupAddr + (entry->stride << 5)), addresses, 0,
                                 PrefetchSourceType::SStride, 2);
                if (is_first_shot) {
                    stats.strideUniquepfCount += 2;
                } else {
                    stats.strideRedundantpfCount += 2;
                }
                if (archDBer) {
                    archDBer->strideTraceWrite(
                        curTick(),
                        blockAddress(lookupAddr + (entry->stride << 2)),
                        pfi.getPC(), stride_hash_pc,
                        true, is_first_shot, pfi.isCacheMiss(), false,
                        triggerSeqNum);
                    archDBer->strideTraceWrite(
                        curTick(),
                        blockAddress(lookupAddr + (entry->stride << 5)),
                        pfi.getPC(), stride_hash_pc,
                        true, is_first_shot, pfi.isCacheMiss(), false,
                        triggerSeqNum);
                }
            } else {
                for (unsigned i = start_depth; i <= entry->depth; i++) {
                    pf_addr = lookupAddr + entry->stride * i;
                    DPRINTF(XSStridePrefetcher, "Stride conf >= 2, send pf: %x with depth %i\n", pf_addr, i);
                    sendPFWithFilter(pfi, blockAddress(pf_addr), addresses, 0, PrefetchSourceType::SStride, 1);
                    if (is_first_shot) {
                        stats.strideUniquepfCount++;
                    } else {
                        stats.strideRedundantpfCount++;
                    }
                }
                stride_pf = pf_addr;  // the longest lookahead
            }

            should_cover = true;
        }
    } else {
        if (is_first_shot) {
            stats.strideUniquemissCount++;
        } else {
            stats.strideRedundantmissCount++;
        }
        DPRINTF(XSStridePrefetcher, "Stride miss, insert it\n");
        entry = stride.findVictim(0);
        DPRINTF(XSStridePrefetcher, "Stride found victim pc = %x, stride = %i\n", entry->pc, entry->stride);
        if (enableNonStrideFilter && (entry->histStrides.size() >= maxHistStrides - 1 || !entry->matchedSinceAlloc)) {
            DPRINTF(XSStridePrefetcher, "Stride hist %u >= %u, mark pc %x as non-stride\n", entry->histStrides.size(),
                    maxHistStrides - 1, entry->pc);
            markNonStridePC(entry->pc);
        }
        if (entry->conf >= 2){
            if (is_first_shot) {
                stats.strideUniquereplaceusefulCount++;
            } else {
                stats.strideRedundantreplaceusefulCount++;
            }
        }
        if (entry->conf >= 2 && entry->stride > 1024) {  // > 1k
            DPRINTF(XSStridePrefetcher, "Stride Evicting a useful stride, send it to BOP with offset %i\n",
                    entry->stride / 64);
            // learnedBOP->tryAddOffset(entry->stride / 64);
            learned_bop_offset = entry->stride / 64;
        }
        entry->conf.reset();
        entry->lastAddr = lookupAddr;
        entry->stride = 0;
        entry->depth = 1;
        entry->lateConf.reset();
        entry->pc = pfi.getPC();
        entry->histStrides.clear();
        entry->matchedSinceAlloc = false;
        DPRINTF(XSStridePrefetcher, "Stride miss, insert with stride 0\n");
        stride.insertEntry(stride_hash_pc, pfi.isSecure(), entry);
    }
    periodStrideDepthDown();
    return should_cover;
}

void
XSStridePrefetcher::periodStrideDepthDown()
{
    if (depthDownCounter < depthDownPeriod) {
        depthDownCounter++;
    } else {
        for (auto stride : {&strideUnique, &strideRedundant}) {
            for (StrideEntry &entry : *stride) {
                if (entry.conf >= 2) {
                    entry.depth = std::max(entry.depth - 1, 1);
                }
            }
        }
        depthDownCounter = 0;
    }
}

void
XSStridePrefetcher::markNonStridePC(Addr pc)
{
    DPRINTF(XSStridePrefetcher, "Mark non-stride pc %x\n", pc);
    auto *entry = nonStridePCs.findEntry(nonStrideHash(pc), false);
    if (entry) {
        nonStridePCs.accessEntry(entry);
    } else {
        entry = nonStridePCs.findVictim(nonStrideHash(pc));
        assert(entry);
        entry->pc = pc;
        nonStridePCs.insertEntry(nonStrideHash(pc), false, entry);
    }
}

bool
XSStridePrefetcher::isNonStridePC(Addr pc)
{
    auto *entry = nonStridePCs.findEntry(nonStrideHash(pc), false);
    return entry != nullptr;
}

void
XSStridePrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
                                      int prio, PrefetchSourceType src, int ahead_level)
{
    const bool pfahead = ahead_level > 1;
    const int pfahead_host = pfahead ? ahead_level : 0;
    recordReplayCandidateTrace(pfi.getAddr(), pfi.getPC(), addr, prio, pfahead,
                               pfahead_host, ahead_level);

    // Count generated prefetch
    prefetchStats.pfGenerated++;
    pfi.setTriggerInfo_PFsrc(src);
    if (ahead_level > 1){
        stridestream_pfFilter_l2l3->Insert(regionAddress(addr), uint64_t(1) << regionOffset(addr),0,true,false,pfi.isSecure(),ahead_level, &pfi.trigger_info);
        if (filterL2->contains(addr)) {
            DPRINTF(XSStridePrefetcher, "Skip recently prefetched: %lx\n", addr);
            // Count filtered prefetch
            prefetchStats.pfFiltered++;
        } else {
            DPRINTF(XSStridePrefetcher, "Send pf: %lx\n", addr);
            filterL2->insert(addr, 0);
            addresses.push_back(AddrPriority(addr, prio, src));
            assert(ahead_level == 2 || ahead_level == 3);
            addresses.back().pfahead_host = ahead_level;
            addresses.back().pfahead = true;
        }
    } else {
        stridestream_pfFilter_l1->Insert(regionAddress(addr), uint64_t(1) << regionOffset(addr),0,true,false,pfi.isSecure(),ahead_level, &pfi.trigger_info);
        if (filter->contains(addr)) {
            DPRINTF(XSStridePrefetcher, "Skip recently prefetched: %lx\n", addr);
            // Count filtered prefetch
            prefetchStats.pfFiltered++;
        } else {
            DPRINTF(XSStridePrefetcher, "Send pf: %lx\n", addr);
            filter->insert(addr, 0);
            addresses.push_back(AddrPriority(addr, prio, src));
            addresses.back().pfahead = false;
        }
    }
}

Addr
XSStridePrefetcher::strideHashPc(Addr pc)
{
    Addr pc_high_1 = (pc >> 20) & (0x1f);
    Addr pc_high_2 = (pc >> 15) & (0x1f);
    Addr pc_high_3 = (pc >> 10) & (0x1f);
    Addr pc_high = pc_high_1 ^ pc_high_2 ^ pc_high_3;
    Addr pc_low = pc & (0x1ff);
    return (pc_high << 10) | pc_low;
}

XSStridePrefetcher::XSstrideStats::XSstrideStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(strideUniquequeryCount, statistics::units::Count::get(), "stride table query num"),
      ADD_STAT(strideUniquehitCount, statistics::units::Count::get(), "stride table hit num"),
      ADD_STAT(strideUniquemissCount, statistics::units::Count::get(), "stride table miss num"),
      ADD_STAT(strideUniquepfCount, statistics::units::Count::get(), "stride prefetch num"),
      ADD_STAT(strideUniquereplaceusefulCount, statistics::units::Count::get(), "stride table replace num"),
      ADD_STAT(strideRedundantqueryCount, statistics::units::Count::get(), "stride table query num"),
      ADD_STAT(strideRedundanthitCount, statistics::units::Count::get(), "stride table hit num"),
      ADD_STAT(strideRedundantmissCount, statistics::units::Count::get(), "stride table miss num"),
      ADD_STAT(strideRedundantpfCount, statistics::units::Count::get(), "stride prefetch num"),
      ADD_STAT(strideRedundantreplaceusefulCount, statistics::units::Count::get(), "stride table replace num"),
      ADD_STAT(commitOrderedS1CaptureCount, statistics::units::Count::get(),
               "successful S1 captures for commit-ordered stride training"),
      ADD_STAT(commitOrderedS1DuplicateCount, statistics::units::Count::get(),
               "duplicate S1 captures skipped by seqNum"),
      ADD_STAT(commitOrderedReadyCount, statistics::units::Count::get(),
               "snapshots marked ready by load commit"),
      ADD_STAT(commitOrderedSquashDropCount, statistics::units::Count::get(),
               "commit-ordered stride snapshots dropped by squash"),
      ADD_STAT(commitOrderedDeferCount, statistics::units::Count::get(),
               "training events deferred because snapshot became ready this cycle"),
      ADD_STAT(commitOrderedTrainDispatchCount, statistics::units::Count::get(),
               "snapshots dispatched from ready queue into trainFromSnapshot"),
      ADD_STAT(commitOrderedTrainEnterCount, statistics::units::Count::get(),
               "entries entering trainFromSnapshot"),
      ADD_STAT(commitOrderedTrainFilteredNonStrideCount,
               statistics::units::Count::get(),
               "train entries skipped because the PC is in the non-stride filter"),
      ADD_STAT(commitOrderedTrainZeroStrideCount, statistics::units::Count::get(),
               "train entries skipped because the new stride delta is zero"),
      ADD_STAT(commitOrderedTrainGuardedCount, statistics::units::Count::get(),
               "train entries skipped by stride guard conditions"),
      ADD_STAT(commitOrderedTrainLongStrideAdjustCount,
               statistics::units::Count::get(),
               "train entries that adjusted longStride saturation state"),
      ADD_STAT(commitOrderedTrainUpdateCount, statistics::units::Count::get(),
               "train entries that updated primary stride entry state"),
      ADD_STAT(commitOrderedTrainAllocCount, statistics::units::Count::get(),
               "train entries that allocated a new stride entry"),
      ADD_STAT(commitOrderedTrainMatchCount, statistics::units::Count::get(),
               "train entries that matched and strengthened an existing stride"),
      ADD_STAT(commitOrderedTrainMismatchCount, statistics::units::Count::get(),
               "train entries that mismatched and decayed an existing stride"),
      ADD_STAT(commitOrderedTrainRetargetCount, statistics::units::Count::get(),
               "train entries that rewrote an existing stride after confidence dropped to zero")
{
}

}

}
