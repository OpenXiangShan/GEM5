
#ifndef __MEM_CACHE_CHISEL_DB_H__
#define __MEM_CACHE_CHISEL_DB_H__

#include <sqlite3.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "base/logging.hh"

namespace gem5{


void init_db(bool en);
void save_db(const char * filename);
void L1MissTrace_write(
  uint64_t pc,
  uint64_t source,
  uint64_t paddr,
  uint64_t vaddr,
  uint64_t stamp,
  const char * site
);

} // namesapce gem5

#endif
