#ifndef __ADDRESSPRED_METADATA_HH_
#define __ADDRESSPRED_METADATA_HH_

#include "base/types.hh"
#include "enums/AddressPredType.hh"

namespace gem5
{

namespace addresspred
{

class APPredMetaData
{
  public:
    Addr pc;
    uint64_t seq_no;
    virtual ~APPredMetaData() {};
};

class APUpdateMetaData
{
  public:
    Addr pc;
    uint64_t seq_no;
    Addr actualAddr;
    bool isMisprediction;
    bool fromDcache;
    virtual ~APUpdateMetaData() {};
};

class APSpecUpdateMetaData
{
  public:
    virtual ~APSpecUpdateMetaData() {};
};

class APResult
{
  public:
    // is address prediction taken?
    bool speculative = false;
    // predicted address
    Addr addr = 0;
};

// This factory class constructs predictor-related data structures
// based on the type of predictor passed in.
class APDataStructFactory
{
  public:
    static APPredMetaData* buildPredMetaData(AddressPredType type);
    static APUpdateMetaData* buildUpdateMetaData(AddressPredType type);
    static APSpecUpdateMetaData* buildSpecUpdateMetaData(AddressPredType type);
};

} // namespace addresspred

} // namespace gem5

#endif // __ADDRESSPRED_METADATA_HH_
