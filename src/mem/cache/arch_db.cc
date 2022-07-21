
#include "arch_db.hh"

bool dump;
sqlite3 *mem_db;
char * zErrMsg;
int rc;


namespace gem5{

static int callback(void *NotUsed, int argc, char **argv, char **azColName){
  return 0;
}

void init_db_L1MissTrace() {
  // create table
  char sql[] = "CREATE TABLE L1MissTrace(" \
    "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
    "PC INT NOT NULL," \
    "SOURCE INT NOT NULL," \
    "PADDR INT NOT NULL," \
    "VADDR INT NOT NULL," \
    "STAMP INT NOT NULL," \
    "SITE TEXT);";
  rc = sqlite3_exec(mem_db, sql, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  } else {
    warn("Table created: L1MissTrace\n");
  }
}

void L1MissTrace_write(
  uint64_t pc,
  uint64_t source,
  uint64_t paddr,
  uint64_t vaddr,
  uint64_t stamp,
  const char * site
) {
  if (!dump) return;
  char sql[512];
  sprintf(sql,
    "INSERT INTO L1MissTrace(PC,SOURCE,PADDR,VADDR, STAMP, SITE) " \
    "VALUES(%ld, %ld, %ld, %ld, %ld, '%s');",
    pc,source,paddr,vaddr, stamp, site
  );
  rc = sqlite3_exec(mem_db, sql, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

void init_db(bool en){
  dump = en;
  if (!en) return;
  rc = sqlite3_open(":memory:", &mem_db);
  if (rc) {
    fatal("Can't open database: %s\n", sqlite3_errmsg(mem_db));
  }
  init_db_L1MissTrace();
}

void save_db(const char *zFilename) {
  warn("saving memdb to %s ...\n", zFilename);
  sqlite3 *disk_db;
  sqlite3_backup *pBackup;
  rc = sqlite3_open(zFilename, &disk_db);
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


} // namespace gem5

