#ifndef __IDEAL_MODEL_HH__
#define __IDEAL_MODEL_HH__

#include <cstdint>
#include <map>
#include <string>

#include "cpu/difftest.hh"

// some defination reference from difftest.hh
enum CommDirection
{
    IDEAL_MODEL_TO_GEM5,
    GEM5_TO_IDEAL_MODEL
};

enum InstFlowRecoverType
{
  RECOVER_BRANCHMISPRED,
  RECOVER_MEMORDERVIOLATION,
  RECOVER_NOTISAFAULT,
  RECOVER_OTHER
};

enum IdealModelWorkState
{
  IM_WORK,
  IM_EXCEPTION_STOP,
  IM_SYSTEMOP_STOP,
  IM_NONSPEC_STOP,
  IM_ITER_STOP
};

enum GEM5CommitType
{
  GEM5_NORMAL,
  GEM5_SINGLECONTROL,
  GEM5_EXCEPTION,
  GEM5_SYSTEMOP,
  GEM5_NOSPEC
};

static const char *IdealModelWorkStateStr[] = {
  "IM_WORK",
  "IM_EXCEPTION_STOP",
  "IM_SYSTEMOP_STOP",
  "IM_NONSPEC_STOP",
  "IM_ITER_STOP"
};

static const char *coloredIdealModelWorkStateStr[] = {
  "\033[32mIM_WORK\033[0m",
  "\033[31mIM_EXCEPTION_STOP\033[0m",
  "\033[31mIM_SYSTEMOP_STOP\033[0m",
  "\033[31mIM_NONSPEC_STOP\033[0m"
  "\033[31mIM_ITER_STOP\033[0m"
};

// define colorful str for instlist dump

// color
#define RESET "\033[0m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN "\033[36m"

// color str
#define COLORIZE(color, text) color text RESET

// map k-v
#define MAKE_PAIR(key, color) {key, COLORIZE(color, key)}

static const std::map<const char*, const char*> coloredInstAttrMap = {
    MAKE_PAIR("squashed", YELLOW),
    MAKE_PAIR("mmio", RED),
    MAKE_PAIR("branch", MAGENTA),
    MAKE_PAIR("exception", RED),
    MAKE_PAIR("systemop", CYAN),
    MAKE_PAIR("nonspec", BLUE),
    MAKE_PAIR("executed", GREEN)
};

// un define
#undef RESET
#undef RED
#undef GREEN
#undef YELLOW
#undef BLUE
#undef MAGENTA
#undef CYAN
#undef COLORIZE
#undef MAKE_PAIR

struct idealModelExecutionGuide
{
    // reference from difftest execution guide
    // force raise exception
    bool force_raise_exception;
    uint64_t exception_num;
    uint64_t mtval;
    uint64_t stval;
    // need enable h
    uint64_t mtval2;
    uint64_t htval;
    uint64_t vstval;
    // force set jump target
    bool force_set_jump_target;
    uint64_t jump_target;
};

// ask from gem5
struct AskFromGEM5
{
  uint64_t seq_no;
  uint64_t pc;

  // now next_pc is not use in nemu ideal model
  uint64_t next_pc;

  // system op: gem5 nonspec and serialize after
  bool is_systemop;

  // nonspec: mmio ll/sc atomic
  bool is_nonspec;

  // load
  bool is_load;

  // store
  bool is_store;

  // need provide dest value
  // different instruction have
  // different value presentation
  // now i want only support value pred-inst
  bool need_provide_dest_value;
};

struct AnswerFromNemu
{
  uint64_t dest_value;
  uint64_t mem_vaddr;
  uint64_t mem_paddr;
  bool is_mmio;
  int ideal_model_work_state;
};

class IdealModelProxy
{
    public:
        IdealModelProxy(const char *ideal_model_so);
        void (*ideal_model_init)() = nullptr;
        void (*ideal_model_memcpy)(paddr_t nemu_addr, void *gem5_buf, size_t n,
                   bool direction) = nullptr;
        void (*ask_ideal_model)(const void *const gem5_info, void *const nemu_info, bool iter_mode) = nullptr;
        void (*adapt_flow_change)(uint64_t seq_no, uint64_t pc, int changeType) = nullptr;
        void (*regcpy)(void *gem5_reg, bool direction) = nullptr;
        void (*ideal_model_exec)(uint64_t n) = nullptr;
        void (*set_intr_happen)() = nullptr;
        void (*clear_intr_happen)() = nullptr;
        void (*gem5_raise_intr)(void *gem5_reg, uint64_t no) = nullptr;
        void (*exception_handle)() = nullptr;
        void (*commit_inst)(int inst_type, uint64_t seq_no, bool is_squash_after) = nullptr;
        void (*raise_runtime_exception)(int inst_type, uint64_t seq_no,
                uint64_t exception_no, bool is_ecall_ebreak) = nullptr;
        void (*ideal_model_guide_exec)(void * guide) = nullptr;
        bool (*raise_force_exception)(uint64_t seq_no, uint64_t exception_no) = nullptr;
        void (*clear_exception_pending)() = nullptr;
        void (*clear_squash_after)() = nullptr;
        void (*set_iter_stop_state)(uint64_t seq_no) = nullptr;
        void (*temp_exec)(void *res_work_state, void *res_pc, uint64_t gem5_pc) = nullptr;
        // void (*ask_ideal_model_itermode)(const void *const gem5_info, void *const nemu_info) = nullptr;
    protected:
        void *handle;
};



#endif

