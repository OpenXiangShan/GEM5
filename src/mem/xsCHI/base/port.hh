#pragma once
#include <cassert>
#include <functional>
#include <memory>
#include <ostream>
#include <string>

#include "base/logging.hh"
#include "base/types.hh"
#include "module.hh"

namespace gem5
{
namespace xsCHI
{

template<typename T>
class Port
{
    private:

    /** Descriptive name (for DPRINTF output) */
    const std::string portName;

  protected:

    class UnboundPortException {};

    [[noreturn]] void reportUnbound() const;

    /**
     * A numeric identifier to distinguish ports in a vector, and set
     * to InvalidPortID in case this port is not part of a vector.
     */
    const PortID id;

    /**
     * Whether this port is currently connected to a peer port.
     */
    bool _connected;


    Port<T>* connected_port;          // 连接的对端Port
    Module* owner_module;             // 所属模块指针
    // std::queue<std::unique_ptr<T>> buffer; // 传输缓冲区
    // uint32_t max_buffer_size;         // 最大缓冲条目数
    // uint32_t bandwidth;               // 传输带宽(GB/s)
    std::function<bool(T&)> receive_callback;
public:

    Port(Module* module, const std::string& _name, PortID _id) :
        portName(_name), id(_id), _connected(false), connected_port(nullptr),
        owner_module(module), receive_callback(nullptr) {
        // 初始化端口
        // max_buffer_size = 0; // 可以根据需要设置
        // bandwidth = 0; // 可以根据需要设置
    }
    ~Port() = default;
    void connect(Port<T>* peer_port) { connected_port = peer_port; }

    bool send(T & data) {
        // 将数据发送到连接的端口
        if (connected_port) {
            // 这里可以添加缓冲逻辑
            return connected_port->receive(data);
        } else {
            panic("Port %s in module %s is not connected to any peer port",
                  owner_module->name(), owner_module->name());
            return false; // 连接失败
        }
    }

    bool receive(T & data)
    {
        // 支持匿名函数回调
        if (receive_callback) {
            return receive_callback(data);
        } else  {
            panic("Receive callback not set for port in module %s", owner_module->name());
            return false; // 连接失败
        }
    }
    void setReceiveCallback(std::function<void(T&)> callback) {
        receive_callback = callback;
    }
    /** Return a reference to this port's peer. */
    Port &getPeer() { return *connected_port; }

    /** Return port name (for DPRINTF). */
    const std::string name() const { return portName; }

    /** Get the port id. */
    PortID getId() const { return id; }

    /** Is this port currently connected to a peer? */
    bool isConnected() const { return _connected; }

    // operator<< will be defined as a friend outside the class
    friend std::ostream& operator<<(std::ostream& os, const Port& port) {
        os << port.name();
        return os;
    }

};

} // namespace xsCHI
} // namespace gem5
