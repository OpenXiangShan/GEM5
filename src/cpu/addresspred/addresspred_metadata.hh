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
    // Squash-version carried by the dynamic instruction itself.
    uint8_t inst_version = 0;
    // Frontend local squash-version when prediction is issued.
    uint8_t squash_version = 0;
    virtual ~APPredMetaData() {};
};

class APUpdateMetaData
{
  public:
    Addr pc;
    uint64_t seq_no;
    Addr actualAddr;
    bool isMisprediction;
    bool apPredictCalled;
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
