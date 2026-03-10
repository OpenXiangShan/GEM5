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
        case ValuePredType::NullPredictor:
            return new VPPredMetaData();
        case ValuePredType::EStride:
            return new ESPredMetaData();
        case ValuePredType::IdealConstantLVP:
        case ValuePredType::MultiValuePredictor:
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
        case ValuePredType::NullPredictor:
            return new VPUpdateMetaData();
        case ValuePredType::EStride:
            return new ESUpdateMetaData();
        case ValuePredType::IdealConstantLVP:
        case ValuePredType::MultiValuePredictor:
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
        case ValuePredType::NullPredictor:
            return new VPSpecUpdateMetaData();
        case ValuePredType::EStride:
            return new ESSpecUpdateMetaData();
        case ValuePredType::IdealConstantLVP:
        case ValuePredType::MultiValuePredictor:
            return new VPSpecUpdateMetaData();
        default:
            assert(0);
    }
    return nullptr;
}


}

}
