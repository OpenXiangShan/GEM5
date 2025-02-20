#include "sim/abort_callback.hh"

#include "base/callback.hh"
#include "base/cprintf.hh"
#include "base/logging.hh"
#include "base/output.hh"

// code reference from core.cc
namespace gem5
{

/**
 * Queue of C++ callbacks to invoke on simulator abort.
 */
inline CallbackQueue &
abortCallbacks()
{
    static CallbackQueue theQueue;
    return theQueue;
}

/**
 * Register an abort callback.
 */
void
registerAbortCallback(const std::function<void()> &callback)
{
    abortCallbacks().push_back(callback);
}

/**
 * Do C++ simulator abort processing.
 */
void
doAbortCleanup()
{
    abortCallbacks().process();
    abortCallbacks().clear();

    std::cout.flush();
}

}

