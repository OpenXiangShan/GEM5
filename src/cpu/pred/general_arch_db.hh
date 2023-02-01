
#ifndef __CPU_PRED_GENERAL_ARCH_DB_H__
#define __CPU_PRED_GENERAL_ARCH_DB_H__

#include <sqlite3.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <map>

#include "base/logging.hh"
#include "base/types.hh"

namespace gem5{
enum DataType
{
    UINT64,
    TEXT,
    OHTER
};

struct Record
{
    Tick _tick;
    std::map<std::string, uint64_t> _uint64_data;
    std::map<std::string, std::string> _text_data;
};

class TraceManager
{

    std::string _name;
    std::map<std::string, DataType> _fields;
    sqlite3 *_db;
public:
    TraceManager(const char *name, std::vector<std::pair<std::string, DataType>> fields, sqlite3 *db) {
        _name = name;
        for (auto it = fields.begin(); it != fields.end(); it++) {
            _fields[it->first] = it->second;
        }
        _db = db;
    }
    TraceManager() {}
    void init_table();
    void write_record(const Record &record);
};

class DataBase
{
    // a trace corrsponds to a table
    std::map<std::string, TraceManager> _traces;
    sqlite3 *mem_db;
    public:
    void init_db();
    void save_db(const char * filename);
    sqlite3 *get_mem_db() {
        return mem_db;
    }

    TraceManager *addAndGetTrace(const char *name, std::vector<std::pair<std::string, DataType>> fields);
};

} // namesapce gem5

#endif
