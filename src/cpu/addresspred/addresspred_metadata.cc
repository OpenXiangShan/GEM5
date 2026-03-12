#include "cpu/addresspred/addresspred_metadata.hh"

#include <cassert>

namespace gem5
{

namespace addresspred
{

APPredMetaData*
APDataStructFactory::buildPredMetaData(AddressPredType type)
{
    switch (type) {
        case AddressPredType::IdealConstantAP:
            return new APPredMetaData();
        case AddressPredType::EStrideAP:
            return new APPredMetaData();
        default:
            assert(0);
    }
    return nullptr;
}

APUpdateMetaData*
APDataStructFactory::buildUpdateMetaData(AddressPredType type)
{
    switch (type) {
        case AddressPredType::IdealConstantAP:
            return new APUpdateMetaData();
        case AddressPredType::EStrideAP:
            return new APUpdateMetaData();
        default:
            assert(0);
    }
    return nullptr;
}

APSpecUpdateMetaData*
APDataStructFactory::buildSpecUpdateMetaData(AddressPredType type)
{
    switch (type) {
        case AddressPredType::IdealConstantAP:
            return new APSpecUpdateMetaData();
        case AddressPredType::EStrideAP:
            return new APSpecUpdateMetaData();
        default:
            assert(0);
    }
    return nullptr;
}

} // namespace addresspred

} // namespace gem5
