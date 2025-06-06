#include "Bridge.hh"

namespace gem5
{
namespace xsCHI
{
    Bridge::Bridge()
        : storagePort(new Port<Request>(this)),
          networkPort(new Port<Flit>(this)) {
            // 初始化存储端口和网络端口
            storagePort->setReceiveCallback(
                [](Request &req) {
                    // 处理存储端口接收到的请求
                    // DPRINTF(Bridge, "Received request on storage port: %s",
                    //         req.toString());
                    // 这里可以添加处理逻辑
                });
            networkPort->setReceiveCallback(
                [](Flit &flit) {
                    // 处理网络端口接收到的Flit
                    // DPRINTF(Bridge, "Received flit on network port: %s",
                    //         flit.toString());
                    // 这里可以添加处理逻辑
                });

          }

    Bridge::~Bridge() = default;


}
}
