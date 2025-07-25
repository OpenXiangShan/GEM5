// plru.hh
// 通用 N-way Pseudo-LRU 替换树结构头文件
#pragma once

#include <vector>
#include <cstddef>

class PLRUTreeN {
private:
    size_t numWays;               // N: cache entry 个数，必须是2的幂
    std::vector<bool> bits;       // 满二叉树内部方向位，大小为 N - 1

public:
    // 构造函数
    explicit PLRUTreeN(size_t ways);

    // 获取应被替换的 victim entry 索引
    size_t getVictim() const;

    // 表示访问了某个 entry，更新树路径
    void access(size_t way);

    // 重置所有方向位为0
    void reset();

    // 返回树支持的容量
    size_t size() const { return numWays; }
};
