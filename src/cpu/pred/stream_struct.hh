#ifndef __CPU_PRED_STREAM_STRUCT_HH__
#define __CPU_PRED_STREAM_STRUCT_HH__

#include <boost/dynamic_bitset.hpp>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/stream_common.hh"

namespace gem5 {

namespace branch_prediction {

enum EndType {
    END_CALL=0,
    END_RET,
    END_OTHER_TAKEN,
    END_NOT_TAKEN,
    END_NONE
};

enum SquashType {
    SQUASH_NONE=0,
    SQUASH_TRAP,
    SQUASH_CTRL
};


using FetchStreamId = uint64_t;
using FetchTargetId = uint64_t;
using PredictionID = uint64_t;

struct FetchStream
{
    Addr streamStart;

    // indicating whether a backing prediction has finished
    bool predEnded;
    bool predTaken;

    // predicted stream end pc
    Addr predEndPC;
    // TODO: use PCState for target(gem5 specific)
    Addr predTarget;
    Addr predBranchPC;
    int predBranchType;

    // for commit, write at redirect or fetch
    bool exeEnded;
    bool exeTaken;
    Addr exeEndPC;
    // TODO: use PCState for target(gem5 specific)
    Addr exeTarget;
    Addr exeBranchPC;
    int endType;
    // TODO: remove signals below
    bool resolved;

    int squashType;

    boost::dynamic_bitset<> history;

    FetchStream()
        : streamStart(0),
          predEnded(false),
          predTaken(false),
          predEndPC(0),
          predTarget(0),
          predBranchPC(0),
          predBranchType(0),
          exeEnded(false),
          exeTaken(false),
          exeEndPC(0),
          exeTarget(0),
          exeBranchPC(0),
          endType(EndType::END_NONE),
          resolved(false),
          squashType(SquashType::SQUASH_NONE)
    {
    }

    // the default exe result should be consistent with prediction
    void setDefaultResolve() {
        resolved = false;
        exeEnded = predEnded;
        exeEndPC = predEndPC;
        exeTarget = predTarget;
        exeBranchPC = predBranchPC;
        // exeBranchType = predBranchType;
    }

    bool getEnded() const { return resolved ? exeEnded : predEnded; }
    Addr getControlPC() const { return resolved ? exeBranchPC : predBranchPC; }
    Addr getEndPC() const { return resolved ? exeEndPC : predEndPC; }
    Addr getTaken() const { return resolved ? exeTaken : predTaken; }
    Addr getTakenTarget() const { return resolved ? exeTarget : predTarget; }
    Addr getFallThruPC() const { return getEndPC(); }
    Addr getNextStreamStart() const {return getTaken() ? getTakenTarget() : getFallThruPC(); }
    bool isCall() const { return endType == END_CALL; }
    bool isReturn() const { return endType == END_RET; }
};

struct FetchingStream : public FetchStream
{
    Addr curPC;
};

struct IdealStreamStorage
{
    // Addr tag;  // addr of the taken branch?
    Addr bbStart;
    Addr controlAddr;
    Addr nextStream;
    uint16_t controlSize;
    unsigned hysteresis;
    bool endNotTaken;
    int endType;
};

struct RealStreamStorage
{
};

using StreamStorage = IdealStreamStorage;


struct StreamPrediction
{
    Addr bbStart;
    Addr controlAddr;
    Addr nextStream;
    uint16_t controlSize;
    bool valid;
    int endType;
    bool endNotTaken;
    bool rasUpdated;
    boost::dynamic_bitset<> history;
    bool isCall() const { return endType == END_CALL; }
    bool isReturn() const { return endType == END_RET; }
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

    // When it is a taken branch, takenPC is the control (starting) PC
    // When it is yet missing, takenPC is the ``known'' PC,
    // decoupledPredict cannot goes beyond takenPC and should be blocked
    // when current PC == takenPC
    Addr takenPC;

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
    
    bool miss() const { return !taken; }
    bool filledUp() const { return (endPC & fetchTargetMask) == 0; }
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
