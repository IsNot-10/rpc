#pragma once

#include <atomic>

namespace metrics {

/**
 * @brief 线程索引分配器类
 * 
 * 为每个线程分配一个唯一的索引，主要用于 LatencyRecorder 中的分片统计
 * 线程安全设计，使用原子操作确保索引分配的唯一性
 */
class ThreadIndex {
public:
    /**
     * @brief 获取当前线程的唯一索引
     * 
     * 使用 static thread_local 初始化机制，确保每个线程只分配一次索引
     * 只有在第一次访问时会调用 Assign() 方法
     * 
     * @return int 当前线程的唯一索引
     */
    static int Get() {
        // 使用 static thread_local 初始化机制，消除每次调用的 if 判断分支
        // 只有在第一次访问时会调用 Assign()
        static thread_local int idx = Assign();
        return idx;
    }
    
    /**
     * @brief 获取已分配的最大索引值
     * 
     * @return int 已分配的最大索引值
     */
    static int Max() {
        return next_index_.load(std::memory_order_relaxed);
    }

private:
    /**
     * @brief 为新线程分配索引
     * 
     * 使用原子操作安全地分配新索引
     * 
     * @return int 分配给新线程的索引
     */
    static int Assign() {
        // 原子递增并获取下一个索引
        int id = next_index_.fetch_add(1, std::memory_order_relaxed);
        
        // 如果线程数超过限制，回绕到 0 (简单的 fallback，防止越界)
        if (id >= 2048) { // 假设最大支持 2048 个线程
            return id % 2048; 
        }
        return id;
    }

    static std::atomic<int> next_index_; ///< 下一个要分配的索引，原子变量确保线程安全
};

} // namespace metrics
