
#include "general_arch_db.hh"

namespace gem5{

static int callback(void *NotUsed, int argc, char **argv, char **azColName){
  return 0;
}

void
TraceManager::init_table() {
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
TraceManager::write_record(const Record &record)
{
    char sql[1024];
    int pos = 0;
    pos = sprintf(sql, "INSERT INTO %s(TICK", _name.c_str());
    for (auto it = _fields.begin(); it != _fields.end(); it++) {
        pos += sprintf(sql+pos, ",%s", it->first.c_str());
    }
    pos += sprintf(sql+pos, ") VALUES(%ld", record._tick);
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
                pos += sprintf(sql+pos, ",%ld", data->second);
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


void
DataBase::init_db(){
  // dump = en;
  // if (!en) return;
  int rc = sqlite3_open(":memory:", &mem_db);
  if (rc) {
    fatal("Can't open database: %s\n", sqlite3_errmsg(mem_db));
  }
  // init_db_L1MissTrace();
}

void
DataBase::save_db(const char *zFilename) {
  warn("saving memdb to %s ...\n", zFilename);
  sqlite3 *disk_db;
  sqlite3_backup *pBackup;
  int rc = sqlite3_open(zFilename, &disk_db);
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

TraceManager *
DataBase::addAndGetTrace(const char *name, std::vector<std::pair<std::string, DataType>> fields)
{
    _traces[name] = TraceManager(name, fields, mem_db);
    return &_traces[name];
}


} // namespace gem5

