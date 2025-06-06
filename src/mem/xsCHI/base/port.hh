#pragma once
#include <functional>
#include <memory>

#include "base/logging.hh"
#include "module.hh"

namespace gem5
{
namespace xsCHI
{

template<typename T>
class Port
{
public:


    // 成员变量
    Port<T>* connected_port;          // 连接的对端Port
    Module* owner_module;             // 所属模块指针
    // std::queue<std::unique_ptr<T>> buffer; // 传输缓冲区
    // uint32_t max_buffer_size;         // 最大缓冲条目数
    // uint32_t bandwidth;               // 传输带宽(GB/s)
    std::function<void(T&)> receive_callback;

    Port(Module* module) : connected_port(nullptr), owner_module(module) {
        // 初始化端口
        // max_buffer_size = 0; // 可以根据需要设置
        // bandwidth = 0; // 可以根据需要设置
    }
    ~Port() = default;
    void connect(Port<T>* peer_port) { connected_port = peer_port; }

    void send(T & data) {
        // 将数据发送到连接的端口
        if (connected_port) {
            // 这里可以添加缓冲逻辑
            connected_port->receive(data);
        }
    }

    void receive(T & data)
    {
        // 支持匿名函数回调
        if (receive_callback) {
            receive_callback(data);
        } else  {
            panic("Receive callback not set for port in module %s", owner_module->name());
        }
    }
    void setReceiveCallback(std::function<void(T&)> callback) {
        receive_callback = callback;
    }
};

} // namespace xsCHI
} // namespace gem5
