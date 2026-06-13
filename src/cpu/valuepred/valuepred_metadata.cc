#include "cpu/valuepred/valuepred_metadata.hh"

#include <cassert>

#include "cpu/valuepred/es_metadata.hh"

namespace gem5
{

namespace valuepred
{

VPPredMetaData*
VPDataStructFactory::buildPredMetaData(ValuePredType type)
{
    switch (type) {
        case ValuePredType::EStride:
            return new ESPredMetaData();
        case ValuePredType::MemoryRenaming:
            return new VPPredMetaData();
        case ValuePredType::IdealConstantLVP:
            return new VPPredMetaData();
        default:
            assert(0);
    }
    return nullptr;
}

VPUpdateMetaData*
VPDataStructFactory::buildUpdateMetaData(ValuePredType type)
{
    switch (type) {
        case ValuePredType::EStride:
            return new ESUpdateMetaData();
        case ValuePredType::MemoryRenaming:
            return new VPUpdateMetaData();
        case ValuePredType::IdealConstantLVP:
            return new VPUpdateMetaData();
        default:
            assert(0);
    }
    return nullptr;
}

VPSpecUpdateMetaData*
VPDataStructFactory::buildSpecUpdateMetaData(ValuePredType type)
{
    switch (type) {
        case ValuePredType::EStride:
            return new ESSpecUpdateMetaData();
        case ValuePredType::MemoryRenaming:
            return new VPSpecUpdateMetaData();
        case ValuePredType::IdealConstantLVP:
            return new VPSpecUpdateMetaData();
        default:
            assert(0);
    }
    return nullptr;
}


}

}
