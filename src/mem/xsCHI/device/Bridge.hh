#pragma once
#include "../base/flit.hh"
#include "../base/module.hh"
#include "../base/port.hh"
#include "../base/request.hh"

namespace gem5
{
namespace xsCHI
{


class Bridge : public Module
{

    private:
        Port<Request> *storagePort; // 存储端口
        Port<Flit> *networkPort;     // 网络端口
    public:
        Bridge();
        ~Bridge();
        Port<Request>* getStoragePort() const { return storagePort; }
        Port<Flit>* getNetworkPort() const { return networkPort; }
    private:



};

// class L2Warper : public Module
// {
//     // ...L2Warper特有成员...
// };
}
}
