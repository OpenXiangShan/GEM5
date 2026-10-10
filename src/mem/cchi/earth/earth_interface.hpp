#pragma once

// Vendored from CHIron: cchi/cohestra/cohestra_earth/earth_interface.hpp
// gem5 adaptation: CHIron includes retargeted (relative "../..." includes in
// the original), expecting -I<CHIRON_DIR>/cchi to be provided at build time;
// otherwise unchanged

#include <cstddef>
#include <optional>
#include <set>

#include "cohestra/cohestra_interface.hpp"

#include "earth_model.hpp"


namespace Cohestra {

    // EarthCCHIInterface: a Cohestra::CCHIInterface implementation that
    // connects the framework directly to a pure-C++ EarthModel as the
    // downstream device, without any Verilated layer in between.
    class EarthCCHIInterface : public CCHIInterface {
    protected:
        EarthModel          model;

    public:
        EarthCCHIInterface(size_t type1PortCount) noexcept;

        virtual ~EarthCCHIInterface() noexcept = default;

    public:
        virtual void    TickPreHandshake(uint64_t time) noexcept override;

    public:
        virtual size_t  GetType1Count() const noexcept override;

        virtual std::set<size_t>
                        GetType1Indices() const noexcept override;

        virtual size_t  GetType1MaxIndex() const noexcept override;

        virtual bool    HasType1SNP(size_t index) const noexcept override;
        virtual std::optional<CCHI::Flits::SNP<CommonFlitConfigurationType1>>
                        PeekType1SNP(size_t index) const noexcept override;
        virtual std::optional<CCHI::Flits::SNP<CommonFlitConfigurationType1>>
                        PopType1SNP(size_t index) noexcept override;

        virtual bool    HasType1DnRSP(size_t index) const noexcept override;
        virtual std::optional<CCHI::Flits::DnRSP<CommonFlitConfigurationType1>>
                        PeekType1DnRSP(size_t index) const noexcept override;
        virtual std::optional<CCHI::Flits::DnRSP<CommonFlitConfigurationType1>>
                        PopType1DnRSP(size_t index) noexcept override;

        virtual bool    HasType1DnDAT(size_t index) const noexcept override;
        virtual std::optional<CCHI::Flits::DnDAT<CommonFlitConfigurationType1>>
                        PeekType1DnDAT(size_t index) const noexcept override;
        virtual std::optional<CCHI::Flits::DnDAT<CommonFlitConfigurationType1>>
                        PopType1DnDAT(size_t index) noexcept override;

        virtual bool    PushType1EVT(size_t index, const CCHI::Flits::EVT<CommonFlitConfigurationType1>& flit) noexcept override;

        virtual bool    PushType1REQ(size_t index, const CCHI::Flits::REQ<CommonFlitConfigurationType1>& flit) noexcept override;

        virtual bool    PushType1UpRSP(size_t index, const CCHI::Flits::UpRSP<CommonFlitConfigurationType1>& flit) noexcept override;

        virtual bool    PushType1UpDAT(size_t index, const CCHI::Flits::UpDAT<CommonFlitConfigurationType1>& flit) noexcept override;

    public:
        EarthModel&         GetModel() noexcept;
        const EarthModel&   GetModel() const noexcept;
    };
}
