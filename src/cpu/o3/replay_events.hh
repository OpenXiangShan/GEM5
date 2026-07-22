#ifndef __CPU_O3_REPLAY_EVENTS_HH__
#define __CPU_O3_REPLAY_EVENTS_HH__

namespace gem5
{
namespace o3
{

enum LdStReplayType
{
    TLBMissReplay,
    CacheMissReplay,
    RescheduleReplay,
    STLFReplay,
    MdpAddrReplay,
    NukeReplay,
    CacheBlockedReplay,
    BankConflictReplay,
    RARReplay,
    RAWReplay,
    MshrAliasFailReplay,
    HitInWriteBufferReplay,
    MshrArbFailReplay,
    /** Wait for the post-translation MPT permission pipeline. */
    MptReplay,
    LdStReplayTypeCount
};

static const char *load_store_replay_event_str[LdStReplayTypeCount] =
{
    "TLBMissReplay",
    "CacheMissReplay",
    "RescheduleReplay",
    "STLFReplay",
    "MdpAddrReplay",
    "NukeReplay",
    "CacheBlockedReplay",
    "BankConflictReplay",
    "RARReplay",
    "RAWReplay",
    "MshrAliasFailReplay",
    "HitInWriteBufferReplay",
    "MshrArbFailReplay",
    "MptReplay",
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_REPLAY_EVENTS_HH__
