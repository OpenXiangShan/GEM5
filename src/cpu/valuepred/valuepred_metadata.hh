#ifndef __VALUEPRED_METADATA_HH_
#define __VALUEPRED_METADATA_HH_

#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

#include "base/types.hh"
#include "enums/ValuePredType.hh"

namespace gem5
{

namespace valuepred
{

// Generic metadata extension map. It allows callers to pass optional fields
// without adding hand-written "union wiring" in MultiValuePredictor.
class VPMetaDataMixin
{
  public:
    using ExtraValue = std::variant<bool, uint64_t, int64_t, double, std::string>;

    template <typename T,
              typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, const char *> &&
                                          !std::is_same_v<std::decay_t<T>, char *>>>
    void setExtraData(const std::string &key, T &&value)
    {
        extraData[key] = std::forward<T>(value);
    }

    void setExtraData(const std::string &key, const char *value)
    {
        extraData[key] = std::string(value);
    }

    void setExtraData(const std::string &key, std::string_view value)
    {
        extraData[key] = std::string(value);
    }

    template <typename T>
    bool getExtraData(const std::string &key, T &value) const
    {
        const auto it = extraData.find(key);
        if (it == extraData.end()) {
            return false;
        }

        const T *typed = std::get_if<T>(&it->second);
        if (!typed) {
            return false;
        }

        value = *typed;
        return true;
    }

  protected:
    void copyExtraDataFrom(const VPMetaDataMixin &other)
    {
        extraData = other.extraData;
    }

  private:
    std::unordered_map<std::string, ExtraValue> extraData;
};

class VPPredMetaData
    : public VPMetaDataMixin
{
  public:
    Addr pc = 0;
    uint64_t seq_no = 0;

    virtual void copyFrom(const VPPredMetaData &other)
    {
        pc = other.pc;
        seq_no = other.seq_no;
        copyExtraDataFrom(other);
    }

    virtual ~VPPredMetaData() {};
};

class VPUpdateMetaData
    : public VPMetaDataMixin
{
  public:
    Addr pc = 0;
    uint64_t seq_no = 0;
    RegVal actualValue = 0;
    bool isMisprediction = false;
    bool hasCandidatePrediction = false;
    RegVal candidateValue = 0;
    ValuePredType predictionSource = ValuePredType::NullPredictor;

    virtual void copyFrom(const VPUpdateMetaData &other)
    {
        pc = other.pc;
        seq_no = other.seq_no;
        actualValue = other.actualValue;
        isMisprediction = other.isMisprediction;
        hasCandidatePrediction = other.hasCandidatePrediction;
        candidateValue = other.candidateValue;
        predictionSource = other.predictionSource;
        copyExtraDataFrom(other);
    }

    virtual ~VPUpdateMetaData() {};
};

class VPSpecUpdateMetaData
    : public VPMetaDataMixin
{
  public:
    virtual void copyFrom(const VPSpecUpdateMetaData &other)
    {
        copyExtraDataFrom(other);
    }

    virtual ~VPSpecUpdateMetaData() {};
};

class VPResult
{
  public:
    // is value prediction taken?
    bool speculative = false;
    // prediction value
    RegVal value = 0;
    // predictor has generated a candidate value (shadow or real)
    bool hasCandidate = false;
    // source predictor type in a composite predictor
    ValuePredType predictionSource = ValuePredType::NullPredictor;
};

// This factory class constructs predictor-related data structures
// based on the type of predictor passed in.
class VPDataStructFactory
{
  public:
    static VPPredMetaData* buildPredMetaData(ValuePredType type);
    static VPUpdateMetaData* buildUpdateMetaData(ValuePredType type);
    static VPSpecUpdateMetaData* buildSpecUpdateMetaData(ValuePredType type);
};

namespace VPCommonMetaKey
{
inline constexpr const char IsLoadInst[] = "vp.is_load_inst";
inline constexpr const char InflightTime[] = "vp.inflight_time";
inline constexpr const char Disassembly[] = "vp.disassembly";
} // namespace VPCommonMetaKey

}

}



#endif
