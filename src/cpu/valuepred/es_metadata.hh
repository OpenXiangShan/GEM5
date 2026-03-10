#ifndef __ES_METADATA_HH__
#define __ES_METADATA_HH__

#include <string>

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
    // debug disassembly text (optional)
    std::string disas;

    void copyFrom(const VPUpdateMetaData &other) override
    {
        VPUpdateMetaData::copyFrom(other);

        // If source is already ES metadata, copy direct fields first.
        if (const auto *esMeta = dynamic_cast<const ESUpdateMetaData *>(&other)) {
            isLoadInst = esMeta->isLoadInst;
            inflightTime = esMeta->inflightTime;
            disas = esMeta->disas;
        }

        // Then apply generic extras as a unified extension channel.
        bool extraIsLoadInst = false;
        if (other.getExtraData(VPCommonMetaKey::IsLoadInst, extraIsLoadInst)) {
            isLoadInst = extraIsLoadInst;
        }

        uint64_t extraInflightTime = 0;
        if (other.getExtraData(VPCommonMetaKey::InflightTime, extraInflightTime)) {
            inflightTime = extraInflightTime;
        }

        std::string extraDisas;
        if (other.getExtraData(VPCommonMetaKey::Disassembly, extraDisas)) {
            disas = std::move(extraDisas);
        }
    }

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
