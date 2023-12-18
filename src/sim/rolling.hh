#ifndef __SIM_ROLLING_H__
#define __SIM_ROLLING_H__

#include <sqlite3.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "base/types.hh"
#include "sim/arch_db.hh"

namespace gem5{

class Rolling
{
  private:
    bool enabled = true;
    Counter interval;
    Counter value;
    Counter base;
    Counter value_interval;
    Counter base_interval;
    ArchDBer *archDBer;
    DBTraceManager *traceManager;

  public:
    Rolling(const char *name, const char *desc = nullptr,
           Counter intv = 1000, ArchDBer *db = nullptr)
    {
      if (db == nullptr || !db->get_dump_rolling()) {
        enabled = false;
        return;
      }

      interval = intv;
      archDBer = db;
      std::vector<std::pair<std::string, DataType>> fields_vec = {
          std::make_pair("value", UINT64),
          std::make_pair("base", UINT64),
      };
      std::string tableName(name);
      tableName += "_rolling";
      traceManager = archDBer->addAndGetTrace(tableName.c_str(), fields_vec);
      traceManager->init_table();
    }

    void operator++(int) { value++; value_interval++; }

    void operator++() { assert(false && "Not implemented\n"); }

    void operator+=(Counter v) { value += v; value_interval += v; }


    Counter get_value_and_clean() {
      Counter temp = value_interval;
      value_interval = 0;
      return temp;
    }

    Counter get_base_and_clean() {
      base_interval = 0;
      return base;
    }

    void roll(Counter v)
    {
      base += v;
      base_interval += v;
      bool dump = (base_interval >= interval);
      if (dump && enabled)
      {
        Counter y_value = get_value_and_clean();
        Counter x_value = get_base_and_clean();
        Record pt;
        pt._tick = curTick() / 500;
        pt._uint64_data["value"] = y_value;
        pt._uint64_data["base"] = x_value;
        traceManager->write_record(pt);
      }
    }
};

} // namespace gem5

#endif