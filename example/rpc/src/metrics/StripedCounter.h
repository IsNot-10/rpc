#pragma once

#include <atomic>
#include <vector>
#include <thread>
#include <functional>

namespace metrics {

/**
 * @brief 高性能计数器类
 * 
 * 实现了类似 Java LongAdder 的设计，通过分散热点 (Striping) 减少多线程竞争
 * 适用于高并发场景下的计数需求
 * 
 * 设计原理：
 * 1. 将一个计数器分散到多个原子变量 (Cell) 中
 * 2. 每个线程根据线程 ID 哈希到不同的 Cell
 * 3. 写入操作只更新对应的 Cell，减少线程间竞争
 * 4. 读取操作需要汇总所有 Cell 的值
 * 5. 使用 Cache Line 对齐，避免 False Sharing
 */
class StripedCounter {
public:
    /**
     * @brief 构造函数
     * 
     * 初始化所有 Cell 为 0
     */
    StripedCounter() {
        for (auto& cell : cells_) {
            cell.store(0, std::memory_order_relaxed); ///< 初始化每个 Cell 为 0
        }
    }

    /**
     * @brief 增加计数值
     * 
     * @param v 增加的值，默认 1
     */
    void Inc(int64_t v = 1) {
        // 根据线程 ID 哈希映射到某个 Cell
        size_t h = std::hash<std::thread::id>{}(std::this_thread::get_id()); ///< 计算线程 ID 的哈希值
        size_t idx = h & (kNumCells - 1); ///< 使用位运算将哈希值映射到 Cell 索引
        cells_[idx].fetch_add(v, std::memory_order_relaxed); ///< 更新对应的 Cell
    }

    /**
     * @brief 获取当前计数值
     * 
     * 汇总所有 Cell 的值，返回总和
     * 
     * @return int64_t 当前计数值
     */
    int64_t Value() const {
        int64_t sum = 0;
        for (const auto& cell : cells_) {
            sum += cell.load(std::memory_order_relaxed); ///< 汇总所有 Cell 的值
        }
        return sum; ///< 返回总和
    }

    /**
     * @brief 重置计数器
     * 
     * 将所有 Cell 重置为 0
     */
    void Reset() {
        for (auto& cell : cells_) {
            cell.store(0, std::memory_order_relaxed); ///< 重置每个 Cell 为 0
        }
    }

private:
    static const size_t kNumCells = 32; ///< Cell 的数量，必须是 2 的幂次方
    
    /**
     * @brief 带缓存行对齐的原子整数
     * 
     * 使用 alignas(64) 确保每个 Cell 独占一个 Cache Line，避免 False Sharing
     */
    struct alignas(64) PaddedAtomic : public std::atomic<int64_t> {
        using std::atomic<int64_t>::atomic; ///< 继承原子整数的构造函数
    };
    
    PaddedAtomic cells_[kNumCells]; ///< Cell 数组，存储计数的多个分片
};

} // namespace metrics
