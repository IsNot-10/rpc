#pragma once

#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <numeric>
#include <algorithm>
#include "ThreadIndex.h"

namespace metrics {

/**
 * @brief 延迟记录器类
 * 
 * 采用无锁分片统计设计，支持高并发场景下的延迟记录
 * 每个线程独占一个分片，避免线程间竞争，提高性能
 * 
 * 设计特点：
 * 1. Lock-Free 设计，每个线程写自己的分片
 * 2. Cache Line 对齐，避免 False Sharing
 * 3. 预定义直方图桶，支持延迟分布统计
 * 4. 支持多线程并发写入，单线程读取聚合
 */
class LatencyRecorder {
    static constexpr size_t kMaxThreads = 128; ///< 支持的最大线程数
    static constexpr size_t kMaxBuckets = 32;  ///< 最大桶数

    /**
     * @brief 分片结构体
     * 
     * 每个线程独占一个分片，用于存储该线程的延迟统计数据
     * Cache Line 对齐，避免 False Sharing
     */
    struct alignas(64) Shard {
        /**
         * @brief 构造函数
         */
        Shard() {
            // 初始化所有桶为 0
            for(size_t i=0; i<kMaxBuckets; ++i) buckets[i].store(0, std::memory_order_relaxed);
        }
        
        // 禁用拷贝和移动构造
        Shard(const Shard&) = delete;
        Shard& operator=(const Shard&) = delete;

        std::atomic<int64_t> count{0};         ///< 延迟记录次数
        std::atomic<int64_t> total_us{0};      ///< 总延迟时间（微秒）
        std::atomic<int64_t> max_latency{0};    ///< 最大延迟时间（微秒）
        std::atomic<int64_t> buckets[kMaxBuckets]; ///< 直方图桶计数
        size_t buckets_size = 0;               ///< 实际使用的桶数
    };

public:
    /**
     * @brief 构造函数
     */
    LatencyRecorder() {
        // 预定义直方图桶边界（微秒）
        buckets_ = {50, 100, 150, 200, 250, 300, 350, 400, 450, 500,
                    600, 700, 800, 900, 1000,
                    2000, 5000, 10000, 50000, 100000, 500000, 1000000};
        
        // 初始化分片数组
        shards_.resize(kMaxThreads);
        for (auto& shard : shards_) {
            shard = std::make_unique<Shard>();          ///< 创建分片实例
            shard->buckets_size = buckets_.size() + 1;   ///< 桶数 = 边界数 + 1（+Inf 桶）
            // 桶已经在 Shard 构造函数中初始化了
        }
    }
    
    /**
     * @brief 记录一次延迟数据
     * 
     * 无锁设计，每个线程写入自己的分片
     * 
     * @param latency_us 延迟时间（微秒）
     */
    void Record(int64_t latency_us) {
        int idx = ThreadIndex::Get(); ///< 获取当前线程的索引
        // 使用 __builtin_expect 优化分支预测，减少分支预测失败的开销
        if (__builtin_expect(idx >= kMaxThreads, 0)) {
            // 超过最大线程数，回退到第0个分片（可能有竞争，但保证安全）
            idx = 0; 
        }
        
        auto& shard = *shards_[idx]; ///< 获取当前线程的分片

        // 1. 更新计数和总延迟
        shard.count.fetch_add(1, std::memory_order_relaxed);       ///< 增加记录次数
        shard.total_us.fetch_add(latency_us, std::memory_order_relaxed); ///< 累加总延迟
        
        // 2. 更新最大值（单写入者，无需 CAS）
        int64_t prev_max = shard.max_latency.load(std::memory_order_relaxed);
        if (latency_us > prev_max) {
            shard.max_latency.store(latency_us, std::memory_order_relaxed);
        }
        
        // 3. 更新直方图（二分查找桶）
        auto it = std::lower_bound(buckets_.begin(), buckets_.end(), latency_us); ///< 二分查找找到对应的桶
        size_t bucket_idx = std::distance(buckets_.begin(), it); ///< 计算桶索引
        
        // 桶索引说明：
        // - 若 latency_us <= buckets[0]，则 bucket_idx = 0
        // - 若 buckets[i] < latency_us <= buckets[i+1]，则 bucket_idx = i+1
        // - 若 latency_us > buckets.back()，则 bucket_idx = buckets_.size()
        
        shard.buckets[bucket_idx].fetch_add(1, std::memory_order_relaxed); ///< 增加对应桶的计数
    }
    
    /**
     * @brief 获取平均延迟
     * 
     * 聚合所有分片的数据，计算平均延迟
     * 
     * @return double 平均延迟（微秒）
     */
    double GetAvgLatency() {
        int64_t total_us = 0; ///< 总延迟时间
        int64_t count = 0;    ///< 总记录次数
        
        // 遍历所有分片，聚合数据
        for (const auto& shard : shards_) {
            total_us += shard->total_us.load(std::memory_order_relaxed); ///< 累加总延迟
            count += shard->count.load(std::memory_order_relaxed);       ///< 累加记录次数
        }
        
        return count > 0 ? (double)total_us / count : 0.0; ///< 计算平均延迟
    }

    /**
     * @brief 获取总记录次数
     * 
     * @return int64_t 总记录次数
     */
    int64_t GetCount() {
        int64_t count = 0;
        // 遍历所有分片，累加记录次数
        for (const auto& shard : shards_) {
            count += shard->count.load(std::memory_order_relaxed);
        }
        return count;
    }

    /**
     * @brief 获取每个桶的计数
     * 
     * @return std::vector<int64_t> 每个桶的计数数组
     */
    std::vector<int64_t> GetCounts() {
        std::vector<int64_t> result(buckets_.size() + 1, 0); ///< 初始化结果数组
        // 遍历所有分片
        for (const auto& shard : shards_) {
            // 遍历所有桶，累加计数
            for (size_t i = 0; i < result.size(); ++i) {
                result[i] += shard->buckets[i].load(std::memory_order_relaxed);
            }
        }
        return result;
    }

    /**
     * @brief 获取桶边界
     * 
     * @return const std::vector<int64_t>& 桶边界数组
     */
    const std::vector<int64_t>& GetBuckets() const {
        return buckets_;
    }

    /**
     * @brief 获取最大延迟
     * 
     * @return int64_t 最大延迟（微秒）
     */
    int64_t GetMaxLatency() {
        int64_t max_lat = 0;
        // 遍历所有分片，找出最大延迟
        for (const auto& shard : shards_) {
            int64_t m = shard->max_latency.load(std::memory_order_relaxed);
            if (m > max_lat) max_lat = m;
        }
        return max_lat;
    }

private:
    std::vector<int64_t> buckets_; ///< 直方图桶边界（微秒）
    // 使用智能指针向量避免 reallocation 导致的问题，且 Shard 大小较大
    std::vector<std::unique_ptr<Shard>> shards_; ///< 分片数组
};

} // namespace metrics
