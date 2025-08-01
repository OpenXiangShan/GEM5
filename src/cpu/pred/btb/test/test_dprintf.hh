#ifndef __CPU_PRED_BTB_TEST_TEST_DPRINTF_HH__
#define __CPU_PRED_BTB_TEST_TEST_DPRINTF_HH__

#include <cstdio>

// Define debug namespace and flags to avoid compilation errors
// Note: Commented out to avoid conflicts with real gem5 debug symbols
// namespace gem5 {
// namespace debug {
//     extern bool BTB;
// }
// }

// Undefine existing macros first to avoid redefinition errors
#ifdef DPRINTF
#undef DPRINTF
#endif

#ifdef DPRINTFS
#undef DPRINTFS
#endif

#ifdef DPRINTFR
#undef DPRINTFR
#endif

#ifdef DPRINTFV
#undef DPRINTFV
#endif

#ifdef DPRINTFN
#undef DPRINTFN
#endif

#ifdef DPRINTFNR
#undef DPRINTFNR
#endif

#ifdef DPRINTF_UNCONDITIONAL
#undef DPRINTF_UNCONDITIONAL
#endif

#define DPRINTF_AS_NOP

#if defined(DPRINTF_AS_NOP)
// define Dprintf as nop
#define DPRINTF(x, ...) {}
#define DPRINTFS(x, s, ...) {}
#define DPRINTFR(x, ...) {}
#define DPRINTFV(x, ...) {}
#define DPRINTFN(...) {}
#define DPRINTFNR(...) {}

#else
// Only define DPRINTF macros if they haven't been defined yet
#ifndef DPRINTF
#define DPRINTF(x, ...) std::printf(__VA_ARGS__)
#endif

#ifndef DPRINTFS
#define DPRINTFS(x, s, ...) std::printf(__VA_ARGS__)
#endif

#ifndef DPRINTFR
#define DPRINTFR(x, ...) std::printf(__VA_ARGS__)
#endif

#ifndef DPRINTFV
#define DPRINTFV(x, ...) std::printf(__VA_ARGS__)
#endif

#ifndef DPRINTFN
#define DPRINTFN(...) std::printf(__VA_ARGS__)
#endif

#ifndef DPRINTFNR
#define DPRINTFNR(...) std::printf(__VA_ARGS__)
#endif

#ifndef DPRINTF_UNCONDITIONAL
#define DPRINTF_UNCONDITIONAL(x, ...) std::printf(__VA_ARGS__)
#endif

#endif

#endif // __CPU_PRED_BTB_TEST_TEST_DPRINTF_HH__
