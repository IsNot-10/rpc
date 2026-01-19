#pragma once
#include <cstdint>
#include <mutex>
#include <vector>
#include <cstddef>

class LalbWeight {
public:
    explicit LalbWeight(int64_t initial_weight);
    
    /**
     * @brief 基于平均延迟初始化权重
     */
    void Seed(double avg_latency, int64_t weight_scale, int64_t min_weight);
    
    struct AddInflightResult { bool chosen; int64_t weight_diff; };
    
    /**
     * @brief 尝试选择该节点 (检查可用权重)
     */
    AddInflightResult AddInflight(size_t index, int64_t dice, int64_t left, int64_t now_us, double punish_inflight_ratio, int64_t min_weight);
    
    int64_t MarkFailed(size_t index, int64_t avg_weight);
    
    /**
     * @brief 基于请求完成反馈更新权重
     */
    int64_t Update(bool success, int64_t begin_time_us, int64_t end_time_us, int64_t timeout_ms, int retried_count, double punish_error_ratio, int64_t weight_scale, int64_t min_weight, double punish_inflight_ratio, size_t index);
    
    void SetWeight(int64_t weight);

    int64_t Disable();
    int64_t Value() const;
    int64_t AvgLatency() const;

private:
    static const int64_t DEFAULT_QPS = 1;
    static const size_t RECV_QUEUE_SIZE = 128;
    
    struct TimeInfo {
        int64_t latency;
        int64_t end_time_us;
    };
    
    int64_t ResetWeight(size_t index, int64_t now_us, double punish_inflight_ratio, int64_t min_weight);
    
    int64_t _weight;        // 当前动态权重
    int64_t _base_weight;   // 基础权重
    mutable std::mutex _mutex;
    int64_t _begin_time_sum;
    int _begin_time_count;
    int64_t _old_diff_sum;
    size_t _old_index;
    int64_t _avg_latency; 
    
    // Circular buffer for TimeInfo
    std::vector<TimeInfo> _time_q;
    size_t _time_q_start = 0; // Index of the oldest element (top)
    size_t _time_q_count = 0; // Number of elements
    int64_t _total_latency = 0; // Sum of latencies in the queue
};
