#ifndef __IDEAL_MODEL_CONFIG_HH__
#define __IDEAL_MODEL_CONFIG_HH__

#include <vector>

#include "base/logging.hh"
#include "enums/IdealModelSupport.hh"

namespace gem5{

class IdealModelConfig
{
    private:
        bool valuePredSupported = false;
        bool loadValuePredSupported = false;
        bool intAddVP = false;
        bool scalarLVP = false;

        void IdealModelConfigEarlyCheck(){
        }
    public:
        IdealModelConfig(const std::vector<IdealModelSupport> &supportTypes){
            gem5_assert(supportTypes.size(), "must choose some ideal model type\n");
            for (IdealModelSupport type: supportTypes){
                switch (type) {
                    case IdealModelSupport::IntAddVP:
                        intAddVP = true;
                        valuePredSupported = true;
                        break;
                    case IdealModelSupport::ScalarLVP:
                        scalarLVP = true;
                        valuePredSupported = true;
                        loadValuePredSupported = true;
                        break;
                    default:
                        gem5_assert(0, "unknown ideal model support type \n");
                }
            }
            IdealModelConfigEarlyCheck();
        }

        bool isValuePredSupported(){
            return valuePredSupported;
        }

        bool isLoadValuePredSupported(){
            return loadValuePredSupported;
        }

        bool isIntAddVP(){
            return intAddVP;
        }

        bool isScalarLVP(){
            return scalarLVP;
        }

};

}



#endif
