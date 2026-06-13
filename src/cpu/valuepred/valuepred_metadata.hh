#ifndef __VALUEPRED_METADATA_HH_
#define __VALUEPRED_METADATA_HH_

#include "base/types.hh"
#include "enums/ValuePredType.hh"

namespace gem5
{

namespace valuepred
{

class VPPredMetaData
{
  public:
    Addr pc;
    uint64_t seq_no;
    ThreadID tid = 0;
    virtual ~VPPredMetaData() {};
};

class VPUpdateMetaData
{
  public:
    Addr pc;
    uint64_t seq_no;
    ThreadID tid = 0;
    RegVal actualValue;
    bool isMisprediction;
    bool hasProducerStorePC = false;
    Addr producerStorePC = 0;
    virtual ~VPUpdateMetaData() {};
};

class VPSpecUpdateMetaData
{
  public:
    ThreadID tid = 0;
    virtual ~VPSpecUpdateMetaData() {};
};

class VPResult
{
  public:
    // is value prediction taken?
    bool speculative = false;
    // prediction value
    RegVal value = 0;
};

// This factory class constructs predictor-related data structures
// based on the type of predictor passed in.
class VPDataStructFactory
{
  public:
    static VPPredMetaData* buildPredMetaData(ValuePredType type);
    static VPUpdateMetaData* buildUpdateMetaData(ValuePredType type);
    static VPSpecUpdateMetaData* buildSpecUpdateMetaData(ValuePredType type);
};

}

}



#endif
