#pragma once

#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <numeric>
#include <algorithm>
#include "ThreadIndex.h"

namespace metrics {

// =================================================================================
// 延迟记录器 (Lock-Free 分片统计)
// =================================================================================
class LatencyRecorder {
    static constexpr size_t kMaxThreads = 128; // 支持的最大线程数
    static constexpr size_t kMaxBuckets = 32;  // 最大桶数

    // Cache Line 对齐，避免 False Sharing
    // 每个线程独占一个 Shard，确保不同线程的写入操作不会争抢同一个 Cache Line
    struct alignas(64) Shard {
        // 使用原子操作保证线程安全（虽然主要是单线程写，但有读线程聚合）
        // 使用 memory_order_relaxed 即可，因为只在聚合时需要可见性
        std::atomic<int64_t> count{0};
        std::atomic<int64_t> total_us{0};
        std::atomic<int64_t> max_latency{0};
        std::atomic<int64_t> buckets[kMaxBuckets];
        size_t buckets_size = 0;

        Shard() {
            for(size_t i=0; i<kMaxBuckets; ++i) buckets[i].store(0, std::memory_order_relaxed);
        }
        
        // Copy/Move disabled
        Shard(const Shard&) = delete;
        Shard& operator=(const Shard&) = delete;
    };

public:
    LatencyRecorder() {
        // 预定义直方图桶 (us)
        buckets_ = {50, 100, 150, 200, 250, 300, 350, 400, 450, 500,
                    600, 700, 800, 900, 1000,
                    2000, 5000, 10000, 50000, 100000, 500000, 1000000};
        
        // 初始化分片
        shards_.resize(kMaxThreads);
        for (auto& shard : shards_) {
            shard = std::make_unique<Shard>();
            shard->buckets_size = buckets_.size() + 1;
            // 桶已经在构造函数中初始化了
        }
    }
    
    // 记录一次延迟数据 (Lock-Free)
    void Record(int64_t latency_us) {
        int idx = ThreadIndex::Get();
        // 使用 __builtin_expect 优化分支预测
        if (__builtin_expect(idx >= kMaxThreads, 0)) {
            // 超过最大线程数，回退到第0个分片（可能有竞争，但保证安全）
            idx = 0; 
        }
        
        auto& shard = *shards_[idx];

        // 1. 更新计数和总和
        shard.count.fetch_add(1, std::memory_order_relaxed);
        shard.total_us.fetch_add(latency_us, std::memory_order_relaxed);
        
        // 2. 更新最大值 (单写入者，无需 CAS)
        int64_t prev_max = shard.max_latency.load(std::memory_order_relaxed);
        if (latency_us > prev_max) {
            shard.max_latency.store(latency_us, std::memory_order_relaxed);
        }
        
        // 3. 更新直方图 (二分查找桶)
        // 优化：对于固定桶，可以使用 std::upper_bound
        auto it = std::lower_bound(buckets_.begin(), buckets_.end(), latency_us);
        size_t bucket_idx = std::distance(buckets_.begin(), it);
        
        // buckets vector size is buckets_.size() + 1 (for +Inf)
        // lower_bound returns first element >= val. 
        // Prometheus buckets are "le" (less than or equal).
        // If val = 50, lower_bound returns 50. bucket index 0 is le 50. Correct.
        // If val = 40, lower_bound returns 50. bucket index 0. Correct.
        // If val = 60, lower_bound returns 100. bucket index 1. Correct.
        // If val > max, returns end. bucket index size. Correct.
        
        shard.buckets[bucket_idx].fetch_add(1, std::memory_order_relaxed);
    }
    
    // 获取平均延迟 (聚合所有分片)
    double GetAvgLatency() {
        int64_t total_us = 0;
        int64_t count = 0;
        
        for (const auto& shard : shards_) {
            total_us += shard->total_us.load(std::memory_order_relaxed);
            count += shard->count.load(std::memory_order_relaxed);
        }
        
        return count > 0 ? (double)total_us / count : 0.0;
    }

    // 获取总计数
    int64_t GetCount() {
        int64_t count = 0;
        for (const auto& shard : shards_) {
            count += shard->count.load(std::memory_order_relaxed);
        }
        return count;
    }

    // 获取桶计数
    std::vector<int64_t> GetCounts() {
        std::vector<int64_t> result(buckets_.size() + 1, 0);
        for (const auto& shard : shards_) {
            for (size_t i = 0; i < result.size(); ++i) {
                result[i] += shard->buckets[i].load(std::memory_order_relaxed);
            }
        }
        return result;
    }

    const std::vector<int64_t>& GetBuckets() const {
        return buckets_;
    }

    int64_t GetMaxLatency() {
        int64_t max_lat = 0;
        for (const auto& shard : shards_) {
            int64_t m = shard->max_latency.load(std::memory_order_relaxed);
            if (m > max_lat) max_lat = m;
        }
        return max_lat;
    }

private:
    std::vector<int64_t> buckets_;
    // 使用指针vector避免reallocation导致的各种问题，且Shard大小较大
    std::vector<std::unique_ptr<Shard>> shards_;
};

} // namespace metrics
