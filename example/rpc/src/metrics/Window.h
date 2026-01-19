#pragma once

#include <vector>
#include <mutex>
#include <chrono>
#include <numeric>
#include <algorithm>

namespace metrics {

// 简单的滑动窗口实现，按时间分桶统计数据
// T: 聚合数据的类型 (例如 int64_t, double)
// Op: 聚合数据的操作 (例如 Sum, Max)
template <typename T>
struct SumOp {
    void operator()(T& lhs, const T& rhs) const { lhs += rhs; }
    T result(const T& val) const { return val; }
};

template <typename T>
struct MaxOp {
    void operator()(T& lhs, const T& rhs) const { if (rhs > lhs) lhs = rhs; }
    T result(const T& val) const { return val; }
};

template <typename T, typename Op = SumOp<T>>
class Window {
public:
    // window_size_ms: 窗口总时长
    // bucket_size_ms: 每个桶的时长
    Window(int window_size_ms = 60000, int bucket_size_ms = 1000)
        : window_size_ms_(window_size_ms), bucket_size_ms_(bucket_size_ms) {
        num_buckets_ = window_size_ms_ / bucket_size_ms_;
        buckets_.resize(num_buckets_);
    }

    void Add(T value) {
        int64_t now = GetCurrentTimeMs();
        int64_t bucket_idx = (now / bucket_size_ms_) % num_buckets_;
        int64_t bucket_time = now / bucket_size_ms_;

        std::lock_guard<std::mutex> lock(mutex_);
        if (buckets_[bucket_idx].second != bucket_time) {
            // 如果是旧周期的桶，重置它
            buckets_[bucket_idx] = {T(), bucket_time};
        }
        op_(buckets_[bucket_idx].first, value);
    }

    T Value() {
        int64_t now = GetCurrentTimeMs();
        int64_t current_bucket_time = now / bucket_size_ms_;
        int64_t min_valid_time = current_bucket_time - num_buckets_ + 1;

        T result = T();
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& bucket : buckets_) {
            if (bucket.second >= min_valid_time && bucket.second <= current_bucket_time) {
                op_(result, bucket.first);
            }
        }
        return op_.result(result);
    }

private:
    int64_t GetCurrentTimeMs() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    }

    int window_size_ms_;
    int bucket_size_ms_;
    int num_buckets_;
    std::vector<std::pair<T, int64_t>> buckets_; // <Value, TimeKey>
    std::mutex mutex_;
    Op op_;
};

} // namespace metrics
