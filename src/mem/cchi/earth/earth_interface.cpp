// Vendored from CHIron: cchi/cohestra/cohestra_earth/earth_interface.cpp
// gem5 adaptation: tracks upstream's two-phase tick rename (Tick ->
// TickPreHandshake); otherwise unchanged apart from the vendored header
// locations

#include "earth_interface.hpp"


// Implementation of: class EarthCCHIInterface
namespace Cohestra {

    EarthCCHIInterface::EarthCCHIInterface(size_t type1PortCount) noexcept
        : model (type1PortCount)
    { }

    void EarthCCHIInterface::TickPreHandshake(uint64_t time) noexcept
    {
        model.Tick(time);
    }

    size_t EarthCCHIInterface::GetType1Count() const noexcept
    {
        return model.GetPortCount();
    }

    std::set<size_t> EarthCCHIInterface::GetType1Indices() const noexcept
    {
        std::set<size_t> indices;

        for (size_t i = 0; i < model.GetPortCount(); ++i)
            indices.insert(i);

        return indices;
    }

    size_t EarthCCHIInterface::GetType1MaxIndex() const noexcept
    {
        return model.GetPortCount() ? model.GetPortCount() - 1 : 0;
    }

    bool EarthCCHIInterface::HasType1SNP(size_t index) const noexcept
    {
        return model.HasSNP(index);
    }

    std::optional<CCHI::Flits::SNP<CommonFlitConfigurationType1>>
    EarthCCHIInterface::PeekType1SNP(size_t index) const noexcept
    {
        return model.PeekSNP(index);
    }

    std::optional<CCHI::Flits::SNP<CommonFlitConfigurationType1>>
    EarthCCHIInterface::PopType1SNP(size_t index) noexcept
    {
        return model.PopSNP(index);
    }

    bool EarthCCHIInterface::HasType1DnRSP(size_t index) const noexcept
    {
        return model.HasDnRSP(index);
    }

    std::optional<CCHI::Flits::DnRSP<CommonFlitConfigurationType1>>
    EarthCCHIInterface::PeekType1DnRSP(size_t index) const noexcept
    {
        return model.PeekDnRSP(index);
    }

    std::optional<CCHI::Flits::DnRSP<CommonFlitConfigurationType1>>
    EarthCCHIInterface::PopType1DnRSP(size_t index) noexcept
    {
        return model.PopDnRSP(index);
    }

    bool EarthCCHIInterface::HasType1DnDAT(size_t index) const noexcept
    {
        return model.HasDnDAT(index);
    }

    std::optional<CCHI::Flits::DnDAT<CommonFlitConfigurationType1>>
    EarthCCHIInterface::PeekType1DnDAT(size_t index) const noexcept
    {
        return model.PeekDnDAT(index);
    }

    std::optional<CCHI::Flits::DnDAT<CommonFlitConfigurationType1>>
    EarthCCHIInterface::PopType1DnDAT(size_t index) noexcept
    {
        return model.PopDnDAT(index);
    }

    bool EarthCCHIInterface::PushType1EVT(size_t index, const CCHI::Flits::EVT<CommonFlitConfigurationType1>& flit) noexcept
    {
        return model.PushEVT(index, flit);
    }

    bool EarthCCHIInterface::PushType1REQ(size_t index, const CCHI::Flits::REQ<CommonFlitConfigurationType1>& flit) noexcept
    {
        return model.PushREQ(index, flit);
    }

    bool EarthCCHIInterface::PushType1UpRSP(size_t index, const CCHI::Flits::UpRSP<CommonFlitConfigurationType1>& flit) noexcept
    {
        return model.PushUpRSP(index, flit);
    }

    bool EarthCCHIInterface::PushType1UpDAT(size_t index, const CCHI::Flits::UpDAT<CommonFlitConfigurationType1>& flit) noexcept
    {
        return model.PushUpDAT(index, flit);
    }

    EarthModel& EarthCCHIInterface::GetModel() noexcept
    {
        return model;
    }

    const EarthModel& EarthCCHIInterface::GetModel() const noexcept
    {
        return model;
    }
}
