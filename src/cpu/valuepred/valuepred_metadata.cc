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
        default:
            assert(0);
    }
}

VPUpdateMetaData*
VPDataStructFactory::buildUpdateMetaData(ValuePredType type)
{
    switch (type) {
        case ValuePredType::EStride:
            return new ESUpdateMetaData();
        default:
            assert(0);
    }
}

VPSpecUpdateMetaData*
VPDataStructFactory::buildSpecUpdateMetaData(ValuePredType type)
{
    switch (type) {
        case ValuePredType::EStride:
            return new ESSpecUpdateMetaData();
        default:
            assert(0);
    }
}


}

}
