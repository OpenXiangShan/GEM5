#ifndef __CPU_PRED_STREAM_STRUCT_HH__
#define __CPU_PRED_STREAM_STRUCT_HH__

#include <list>

#include <boost/dynamic_bitset.hpp>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/stream/stream_common.hh"

namespace gem5 {

namespace branch_prediction {

namespace stream_pred {

enum EndType {
    END_CALL=0,
    END_RET,
    END_OTHER_TAKEN,
    END_NOT_TAKEN,
    END_CONT,  // to be continued
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

struct DivideEntry
{
    bool taken;
    Addr start;
    Addr branch;
    Addr next;
    Addr fallThruPC;

    DivideEntry() : taken(false), start(0), branch(0), next(0), fallThruPC(0) {}
    DivideEntry(bool taken, Addr start, Addr branch, Addr next, Addr fallThruPC) : taken(taken), start(start), branch(branch), next(next),
                                                                                   fallThruPC(fallThruPC) {}

};

struct LoopEntry
{
    // may need to add a valid bit
    bool valid;
    Addr branch;
    Addr target;
    Addr outTarget;
    Addr fallThruPC;
    int tripCount;
    int detectedCount;
    bool intraTaken;
    unsigned age;

    LoopEntry() : valid(true), branch(0), target(0), outTarget(0), fallThruPC(0), tripCount(0), detectedCount(0), intraTaken(false), age(3) {}
    LoopEntry(Addr branch, Addr target, Addr outTarget, Addr fallThruPC, int detectedCount, bool intraTaken) :
              valid(true), branch(branch), target(target), outTarget(outTarget), fallThruPC(fallThruPC),
              tripCount(0), detectedCount(detectedCount), intraTaken(intraTaken), age(3) {}
};

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
    int tripCount;
    bool isMiss;
    bool useLoopPrediction;
    bool isLoop;
    Addr loopTarget;
    Addr tageTarget;
    unsigned predSource;
    std::list<std::pair<Addr, unsigned int>> mruLoop;

    boost::dynamic_bitset<> history;

    std::vector<boost::dynamic_bitset<>> indexFoldedHist;
    std::vector<boost::dynamic_bitset<>> tagFoldedHist;

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
          squashType(SquashType::SQUASH_NONE),
          tripCount(-1),
          isMiss(false),
          useLoopPrediction(false),
          isLoop(false),
          loopTarget(0),
          tageTarget(0),
          predSource(0)
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

struct StreamDesc
{

    Addr bbStart;
    Addr controlAddr;
    Addr nextStream;
    uint16_t controlSize;
    int endType;

    bool isTaken()
    {
        return endType == END_OTHER_TAKEN || endType == END_CALL ||
               endType == END_RET;
    }
    bool toBeCont() { return endType == END_CONT; }
    bool isCall() const { return endType == END_CALL; }
    bool isReturn() const { return endType == END_RET; }
    Addr getFallThruPC() const { return controlAddr + controlSize; }
};

struct IdealStreamStorage: public StreamDesc
{
    int hysteresis;
};

struct RealStreamStorage: public StreamDesc
{
};

using StreamStorage = IdealStreamStorage;


struct StreamPrediction: public StreamDesc
{
    bool valid;
    bool useLoopPrediction;
    Addr tageTarget;
    unsigned predSource;
    boost::dynamic_bitset<> history;

    bool match(StreamPrediction &other)
    {
        if (!other.valid) {
            // chosen is invalid, means all predictors invalid, match
            return true;
        } else {
            // chosen is valid
            if (!this->valid) {
                // if this is invalid and chosen is valid, sure no match
                return false;
            } else {
                // both this and chosen valid
                if (this->isTaken() != other.isTaken()) {
                    return false;
                } else if (this->isTaken() && other.isTaken()) {
                    // here bb.start is not a compare criteria, since ubtb and tage's bbStart is different
                    return controlAddr == other.controlAddr && nextStream == other.nextStream;   // TODO consider endType and controlAddr
                } else {
                    return controlAddr == other.controlAddr && controlSize == other.controlSize && endType == other.endType;

                }
            }
        }
    }

    std::vector<boost::dynamic_bitset<>> indexFoldedHist;
    std::vector<boost::dynamic_bitset<>> tagFoldedHist;
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
    unsigned predLoopIteration;
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

}  // namespace stream_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_STREAM_STRUCT_HH__
