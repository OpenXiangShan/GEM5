/*
 * Copyright (c) 2026
 *
 * Alias for the verilated CCHI downstream top module. Only available when
 * the build enables WITH_CCHI_RTL (see SConscript): CCHI_RTL_TOP_HEADER and
 * CCHI_RTL_TOP_CLASS come from the build as -D defines and resolve to the
 * generated model for the configured CCHI_RTL_TOP.
 */

#ifndef __MEM_CCHI_CCHI_RTL_TOP_HH__
#define __MEM_CCHI_CCHI_RTL_TOP_HH__

#ifdef CCHI_RTL_ENABLED

// Generated per build variant by the SConscript: includes verilated.h and
// the verilated top's header, and aliases it as gem5::CchiRtlModule
#include "cchi_rtl_top_generated.hh"

#endif // CCHI_RTL_ENABLED

#endif // __MEM_CCHI_CCHI_RTL_TOP_HH__
