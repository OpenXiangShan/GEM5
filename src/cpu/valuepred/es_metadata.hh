#ifndef __ES_METADATA_HH__
#define __ES_METADATA_HH__

#include <string_view>

#include "cpu/valuepred/valuepred_metadata.hh"

namespace gem5
{

namespace valuepred
{

class ESPredMetaData : public VPPredMetaData
{
  public:
    virtual ~ESPredMetaData() {};
};

class ESUpdateMetaData : public VPUpdateMetaData
{
  public:
    bool isLoadInst = false;
    uint64_t inflightTime = 0;
    // for debug
    std::string_view disas;

    virtual ~ESUpdateMetaData() {};
};

class ESSpecUpdateMetaData : public VPSpecUpdateMetaData
{
  public:
    virtual ~ESSpecUpdateMetaData() {};
};

}

}


#endif
