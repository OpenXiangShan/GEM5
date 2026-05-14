#include <sstream>

#include "general_arch_db.hh"

namespace gem5{

static int callback(void *NotUsed, int argc, char **argv, char **azColName){
  return 0;
}

namespace
{

long long
sqliteSignedInt(uint64_t value)
{
    return static_cast<long long>(static_cast<int64_t>(value));
}

} // anonymous namespace

void
TraceManager::init_table() {
  std::ostringstream sql;
  sql << "CREATE TABLE " << _name
      << "("
      << "ID INTEGER PRIMARY KEY AUTOINCREMENT, "
      << "TICK INT NOT NULL";
  for (auto it = _fields.begin(); it != _fields.end(); it++) {
    switch (it->second) {
      case UINT64:
        sql << "," << it->first << " INT NOT NULL";
        break;
      case TEXT:
        sql << "," << it->first << " TEXT";
        break;
      default:
        fatal("Unknown data type");
    }
  }
  sql << ");";
  const auto sqlStr = sql.str();
  printf("%s\n", sqlStr.c_str());
  char *zErrMsg;
  int rc = sqlite3_exec(_db, sqlStr.c_str(), callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  } else {
    warn("Table created: %s\n", _name.c_str());
  }
}

void
TraceManager::write_record(const Record &record)
{
    std::ostringstream sql;
    sql << "INSERT INTO " << _name << "(TICK";
    for (auto it = _fields.begin(); it != _fields.end(); it++) {
        sql << "," << it->first;
    }
    sql << ") VALUES(" << sqliteSignedInt(record._tick);
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
                sql << "," << sqliteSignedInt(data->second);
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
                sql << ",'" << data->second << "'";
                break;
            }
            default:
                fatal("Unknown data type!\n");
        }
    }
    sql << ");";
    const auto sqlStr = sql.str();
    char *zErrMsg;
    int rc = sqlite3_exec(_db, sqlStr.c_str(), callback, 0, &zErrMsg);
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
