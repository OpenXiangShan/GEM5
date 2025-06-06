#pragma once
#include <string>

namespace gem5
{
namespace xsCHI
{


class Module
{
public:
    virtual ~Module() = default;
    // 至少包含一个Port
    // Port* port;
    // ...其他通用成员...
    std::string name() const { return _name; }
protected:
    std::string _name; // 模块名称
    //cycle event
    //getPort() const { return port_a; } // 获取端口的虚函数
    //
};

// class L2Warper : public Module
// {
//     // ...L2Warper特有成员...
// };

// class Bridge : public Module
// {
//     // Port* storagePort;
//     // Port* networkPort;
//     // ...Bridge特有成员...
// };

// class DDRWarper : public Module
// {
//     // ...DDRWarper特有成员...
// };

// class MeshNode : public Module
// {
//     // 四个方向的FlitPort和两个device端口
//     // ...MeshNode特有成员...
// };

// class LLC : public Module
// {
//     // L3缓存、SnoopFilter、PointOfCoherenceQueue等
//     // ...LLC特有成员...
// };

} // namespace xsCHI
} // namespace gem5
