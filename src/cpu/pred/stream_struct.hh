#ifndef __CPU_PRED_STREAM_STRUCT_HH__
#define __CPU_PRED_STREAM_STRUCT_HH__

#include <boost/dynamic_bitset.hpp>

#include "base/types.hh"
#include "cpu/inst_seq.hh"

namespace gem5 {

namespace branch_prediction {

using FetchStreamId = uint64_t;
using FetchTargetId = uint64_t;
using PredictionID = uint64_t;

struct FetchStream
{
    Addr streamStart;

    // indicating whether a backing prediction has finished
    bool streamEnded;

    // predicted stream end pc
    Addr predStreamEnd;
    // TODO: use PCState for target(gem5 specific)
    Addr predTarget;
    Addr predBranchAddr;
    int predBranchType;

    // for commit, write at redirect or fetch
    InstSeqNum branchSeq;
    bool exeEnded;
    Addr exeStreamEnd;
    // TODO: use PCState for target(gem5 specific)
    Addr exeTarget;
    Addr exeBranchAddr;
    int exeBranchType;
    // TODO: remove signals below
    bool hasEnteredFtq;

    boost::dynamic_bitset<> history;

    FetchStream()
        : streamStart(0)
        , streamEnded(false)
        , predStreamEnd(0)
        , predTarget(0)
        , predBranchAddr(0)
        , predBranchType(0)
        , branchSeq(-1)
        , exeEnded(false)
        , exeStreamEnd(0)
        , exeTarget(0)
        , exeBranchAddr(0)
        , exeBranchType(0)
        , hasEnteredFtq(0) {}

    // the default exe result should be consistent with prediction
    void setDefaultResolve() {
        exeEnded = streamEnded;
        exeStreamEnd = predStreamEnd;
        exeTarget = predTarget;
        exeBranchAddr = predBranchAddr;
        exeBranchType = predBranchType;
    }
};

struct FetchingStream : public FetchStream
{
    Addr curPC;
};

struct IdealStreamStorage
{
    // Addr tag;  // addr of the taken branch?
    Addr bbStart;
    Addr bbEnd;
    Addr nextStream;
    unsigned length;
    // unsigned instCount;
    unsigned hysteresis;
    bool endIsRet;
};

struct RealStreamStorage
{
};

using StreamStorage = IdealStreamStorage;


struct StreamPrediction
{
    Addr bbStart;
    Addr bbEnd;
    Addr nextStream;
    uint16_t streamLength;
    bool valid;
    bool endIsRet;
    bool rasUpdated;
    boost::dynamic_bitset<> history;
};

struct StreamPredictionWithID : public StreamPrediction
{
    PredictionID id;

    StreamPredictionWithID(const StreamPrediction &pred, PredictionID id)
        : StreamPrediction(pred), id(id) {}
};

using StreamLen = uint16_t;
#define unlimitedStreamLen (std::numeric_limits<StreamLen>::max())

// each entry corrsponds to a cache line
struct FtqEntry
{
    Addr startPC;
    Addr endPC;    // TODO: use PCState and it can be included in takenPC
    Addr takenPC;  // TODO: use PCState
    bool taken;
    Addr target;  // TODO: use PCState
    FetchStreamId fsqID;
    FtqEntry()
        : startPC(0)
        , endPC(0)
        , takenPC(0)
        , taken(false)
        , target(0)
        , fsqID(0) {}
};

// struct FetchStreamWithID: public FetchStream {
//     FsqID id;
//     bool operator==(const FetchStreamWithID &other) const {
//         return id == other.id;
//     }
//     FetchStreamWithID(const FetchStream &stream, FsqID id) :
//     FetchStream(stream), id(id) {}
// }

// struct FtqEntryWithID: public FtqEntry {
//     FtqID id;
//     bool operator==(const FtqEntryWithID &other) const {
//         return id == other.id;
//     }
//     FtqEntryWithID(const FtqEntry &entry, FtqID id) : FtqEntry(entry),
//     id(id) {}
// }

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_STREAM_STRUCT_HH__
