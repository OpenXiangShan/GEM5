#pragma once
#include <memory>

namespace gem5
{
namespace xsCHI
{

class Request : public std::enable_shared_from_this<Request>
{
protected:
    // 事务状态跟踪
    enum class TransactionState
    {
        Pending, Snooping, Responded, Completed };

public:
    // 成员变量
    uint64_t transaction_id;          // 唯一事务标识符
    uint32_t source_id;               // 请求源节点ID
    uint32_t target_id;               // 目标节点ID
    TransactionState current_state;   // 当前事务状态
    // std::vector<uint64_t> address_range; // 访问的地址范围
    // uint8_t opcode;                   // CHI操作码
    // uint32_t data_size;               // 关联数据大小(字节)
    // uint32_t qos_priority;            // 服务质量优先级
};
using ReqPtr = std::shared_ptr<Request>;
} // namespace xsCHI
} // namespace gem5
