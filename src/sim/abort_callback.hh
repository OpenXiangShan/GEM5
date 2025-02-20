#ifndef __ABORT_CALLBACK_HH__
#define __ABORT_CALLBACK_HH__

#include <functional>

namespace gem5
{

// code reference from core.hh
void registerAbortCallback(const std::function<void()> &callback);
void doAbortCleanup();


}



#endif
