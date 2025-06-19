#pragma once
#include <bitset>
#include <string>

#include "sim/clocked_object.hh"

namespace gem5
{
namespace xsCHI
{


class Module : public ClockedObject
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



} // namespace xsCHI
} // namespace gem5
