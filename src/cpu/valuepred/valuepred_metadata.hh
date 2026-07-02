#ifndef __VALUEPRED_METADATA_HH_
#define __VALUEPRED_METADATA_HH_

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/types.hh"
#include "enums/ValuePredType.hh"

namespace gem5
{

namespace valuepred
{

class VPPredictRequestExtension
{
  public:
    virtual ~VPPredictRequestExtension() = default;
};

class VPUpdateInfoExtension
{
  public:
    virtual ~VPUpdateInfoExtension() = default;
};

template <typename BaseExt>
class ExtensionContainer
{
  public:
    template <typename Ext, typename... Args>
    Ext &
    emplace(Args&&... args)
    {
        static_assert(std::is_base_of_v<BaseExt, Ext>,
                "Extension type must derive from the container base");
        auto ext = std::make_unique<Ext>(std::forward<Args>(args)...);
        auto *ptr = ext.get();
        extensions.emplace_back(std::move(ext));
        return *ptr;
    }

    template <typename Ext>
    Ext *
    get()
    {
        static_assert(std::is_base_of_v<BaseExt, Ext>,
                "Extension type must derive from the container base");
        for (auto &ext : extensions) {
            if (auto *typed = dynamic_cast<Ext *>(ext.get())) {
                return typed;
            }
        }
        return nullptr;
    }

    template <typename Ext>
    const Ext *
    get() const
    {
        static_assert(std::is_base_of_v<BaseExt, Ext>,
                "Extension type must derive from the container base");
        for (const auto &ext : extensions) {
            if (auto *typed = dynamic_cast<const Ext *>(ext.get())) {
                return typed;
            }
        }
        return nullptr;
    }

  private:
    std::vector<std::unique_ptr<BaseExt>> extensions;
};

class VPPredictRequest
{
  public:
    Addr pc = 0;
    uint64_t seqNo = 0;
    ThreadID tid = 0;

    template <typename Ext, typename... Args>
    Ext &
    emplaceExt(Args&&... args)
    {
        return extensions.emplace<Ext>(std::forward<Args>(args)...);
    }

    template <typename Ext>
    Ext *
    getExt()
    {
        return extensions.get<Ext>();
    }

    template <typename Ext>
    const Ext *
    getExt() const
    {
        return extensions.get<Ext>();
    }

  private:
    ExtensionContainer<VPPredictRequestExtension> extensions;
};

class VPUpdateInfo
{
  public:
    Addr pc = 0;
    uint64_t seqNo = 0;
    ThreadID tid = 0;
    RegVal actualValue = 0;
    bool isMisprediction = false;

    template <typename Ext, typename... Args>
    Ext &
    emplaceExt(Args&&... args)
    {
        return extensions.emplace<Ext>(std::forward<Args>(args)...);
    }

    template <typename Ext>
    Ext *
    getExt()
    {
        return extensions.get<Ext>();
    }

    template <typename Ext>
    const Ext *
    getExt() const
    {
        return extensions.get<Ext>();
    }

  private:
    ExtensionContainer<VPUpdateInfoExtension> extensions;
};

class ProducerInfoExt : public VPUpdateInfoExtension
{
  public:
    explicit ProducerInfoExt(Addr producer_store_pc)
        : producerStorePC(producer_store_pc)
    {
    }

    Addr producerStorePC = 0;
};

class VPSpecUpdateInfo
{
  public:
    ThreadID tid = 0;
};

class VPPredictionRecord
{
  public:
    virtual ~VPPredictionRecord() = default;

    bool offeredPrediction = false;
    RegVal predictedValue = 0;
};

class VPResult
{
  public:
    // is value prediction taken?
    bool speculative = false;
    // prediction value
    RegVal value = 0;
};

class VPPredictionCandidate
{
  public:
    VPResult result = {};
    uint64_t score = 0;
    std::unique_ptr<VPPredictionRecord> record;
};

class VPChooserCandidate
{
  public:
    int childIndex = -1;
    ValuePredType predictorType = ValuePredType::EStride;
    bool speculative = false;
    RegVal value = 0;
    uint64_t score = 0;
};

class VPChooserPolicy
{
  public:
    virtual ~VPChooserPolicy() = default;

    // Return the selected child index, or -1 if no prediction is accepted.
    virtual int choose(const std::vector<VPChooserCandidate> &candidates) const = 0;
};

class VPFeedback
{
  public:
    bool selected = false;
    bool applied = false;
    bool offeredPrediction = false;
    RegVal predictedValue = 0;
    bool wouldHaveBeenCorrect = false;
};

class VPChooserFeedback
{
  public:
    int childIndex = -1;
    bool selected = false;
    bool offeredPrediction = false;
    bool wouldHaveBeenCorrect = false;
};
}

}



#endif
