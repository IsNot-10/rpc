#pragma once

#include <vector>
#include <mutex>
#include <chrono>
#include <numeric>
#include <algorithm>

namespace metrics {

/**
 * @brief 求和操作结构体
 * 
 * 用于滑动窗口的求和聚合
 * 	param T 数据类型
 */
template <typename T>
struct SumOp {
    /**
     * @brief 执行求和操作
     * 
     * @param lhs 左操作数，结果将存储在此处
     * @param rhs 右操作数
     */
    void operator()(T& lhs, const T& rhs) const { lhs += rhs; }
    
    /**
     * @brief 获取结果
     * 
     * @param val 当前值
     * @return T 结果值
     */
    T result(const T& val) const { return val; }
};

/**
 * @brief 最大值操作结构体
 * 
 * 用于滑动窗口的最大值聚合
 * 	param T 数据类型
 */
template <typename T>
struct MaxOp {
    /**
     * @brief 执行最大值操作
     * 
     * @param lhs 左操作数，结果将存储在此处
     * @param rhs 右操作数
     */
    void operator()(T& lhs, const T& rhs) const { if (rhs > lhs) lhs = rhs; }
    
    /**
     * @brief 获取结果
     * 
     * @param val 当前值
     * @return T 结果值
     */
    T result(const T& val) const { return val; }
};

/**
 * @brief 滑动窗口类
 * 
 * 简单的滑动窗口实现，按时间分桶统计数据
 * 支持不同的聚合操作（求和、最大值等）
 * 	param T 聚合数据的类型（例如 int64_t, double）
 * 	param Op 聚合数据的操作，默认是 SumOp<T>（求和）
 */
template <typename T, typename Op = SumOp<T>>
class Window {
public:
    /**
     * @brief 构造函数
     * 
     * @param window_size_ms 窗口总时长（毫秒），默认 60000ms（1分钟）
     * @param bucket_size_ms 每个桶的时长（毫秒），默认 1000ms（1秒）
     */
    Window(int window_size_ms = 60000, int bucket_size_ms = 1000)
        : window_size_ms_(window_size_ms), bucket_size_ms_(bucket_size_ms) {
        num_buckets_ = window_size_ms_ / bucket_size_ms_; ///< 计算桶的数量
        buckets_.resize(num_buckets_); ///< 初始化桶数组
    }

    /**
     * @brief 添加数据到滑动窗口
     * 
     * @param value 要添加的数据值
     */
    void Add(T value) {
        int64_t now = GetCurrentTimeMs(); ///< 获取当前时间（毫秒）
        int64_t bucket_idx = (now / bucket_size_ms_) % num_buckets_; ///< 计算当前桶索引
        int64_t bucket_time = now / bucket_size_ms_; ///< 计算当前桶的时间键

        std::lock_guard<std::mutex> lock(mutex_); ///< 加锁保护临界区
        if (buckets_[bucket_idx].second != bucket_time) {
            // 如果桶的时间键不匹配，说明是旧周期的桶，重置它
            buckets_[bucket_idx] = {T(), bucket_time}; ///< 重置桶数据
        }
        op_(buckets_[bucket_idx].first, value); ///< 应用聚合操作
    }

    /**
     * @brief 获取滑动窗口的聚合结果
     * 
     * @return T 聚合结果
     */
    T Value() {
        int64_t now = GetCurrentTimeMs(); ///< 获取当前时间（毫秒）
        int64_t current_bucket_time = now / bucket_size_ms_; ///< 计算当前桶的时间键
        int64_t min_valid_time = current_bucket_time - num_buckets_ + 1; ///< 计算最小有效时间键

        T result = T(); ///< 初始化结果
        std::lock_guard<std::mutex> lock(mutex_); ///< 加锁保护临界区
        for (const auto& bucket : buckets_) {
            // 只处理在有效时间范围内的桶
            if (bucket.second >= min_valid_time && bucket.second <= current_bucket_time) {
                op_(result, bucket.first); ///< 应用聚合操作
            }
        }
        return op_.result(result); ///< 返回聚合结果
    }

private:
    /**
     * @brief 获取当前时间（毫秒）
     * 
     * @return int64_t 当前时间（毫秒）
     */
    int64_t GetCurrentTimeMs() {
        auto now = std::chrono::steady_clock::now(); ///< 获取当前时间点
        // 转换为毫秒
        return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    }

    int window_size_ms_; ///< 窗口总时长（毫秒）
    int bucket_size_ms_; ///< 每个桶的时长（毫秒）
    int num_buckets_; ///< 桶的数量
    std::vector<std::pair<T, int64_t>> buckets_; ///< 桶数组，每个桶是 <值, 时间键> 对
    std::mutex mutex_; ///< 保护临界区的互斥锁
    Op op_; ///< 聚合操作对象
};

} // namespace metrics
