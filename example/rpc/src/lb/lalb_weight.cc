#include "lb/lalb_weight.h"
#include "lb/lb_common.h"
#include "lb/lalb_manager.h"
#include <algorithm>
#include <cmath>
#include <chrono>

using namespace lb; ///< 使用 lb 命名空间

/**
 * @brief 延迟感知负载均衡权重类的构造函数
 * 
 * @param initial_weight 初始权重值
 */
LalbWeight::LalbWeight(int64_t initial_weight)
    : shared_weight(initial_weight), _weight(initial_weight), _base_weight(initial_weight),
      _begin_time_sum(0), _begin_time_count(0),
      _old_diff_sum(0), _old_index((size_t)-1),
      _avg_latency(0) {
    _time_q.resize(RECV_QUEUE_SIZE); ///< 初始化时间信息循环缓冲区
}

/**
 * @brief 基于平均延迟初始化权重
 * 
 * @param avg_latency 平均延迟值（微秒）
 */
void LalbWeight::Seed(double avg_latency) {
    std::lock_guard<std::mutex> lk(_mutex); ///< 加锁保护临界区
    if (avg_latency > 0) {
        _avg_latency = (int64_t)avg_latency; ///< 设置平均延迟
        // 限制延迟下限，避免对于极快本地调用产生极端权重
        int64_t safe_latency = std::max<int64_t>(_avg_latency, MIN_LATENCY_US); 
        _base_weight = LalbManager::WEIGHT_SCALE / safe_latency; ///< 计算基础权重
        if (_base_weight < LalbManager::MIN_WEIGHT) _base_weight = LalbManager::MIN_WEIGHT; ///< 确保权重不低于最小值
        _weight = _base_weight; ///< 设置当前权重
    }
}

/**
 * @brief 尝试选择该节点
 * 
 * 检查节点的可用权重，决定是否选择该节点
 * 
 * @param index 节点索引
 * @param dice 随机数
 * @param left 剩余权重
 * @param now_us 当前时间（微秒）
 * @return AddInflightResult 选择结果，包含是否选中和权重差异
 */
LalbWeight::AddInflightResult LalbWeight::AddInflight(size_t index, int64_t dice, int64_t left, int64_t now_us) {
    std::lock_guard<std::mutex> lk(_mutex); ///< 加锁保护临界区
    if (_base_weight < 0) { ///< 节点已被禁用
        return {false, 0};
    }
    const int64_t diff = ResetWeight(index, now_us); ///< 重置权重
    if (_weight < (dice - left)) { ///< 权重不足，无法选择
        return {false, diff};
    }
    _begin_time_sum += now_us; ///< 累加开始时间
    ++_begin_time_count; ///< 增加在飞请求计数
    return {true, diff}; ///< 选择成功
}

/**
 * @brief 标记节点失败
 * 
 * 当节点调用失败时，更新权重
 * 
 * @param index 节点索引
 * @param avg_weight 平均权重
 * @return int64_t 权重变化量
 */
int64_t LalbWeight::MarkFailed(size_t index, int64_t avg_weight) {
    std::lock_guard<std::mutex> lk(_mutex); ///< 加锁保护临界区
    if (_base_weight <= avg_weight) { ///< 当前权重已低于或等于平均权重，无需更新
        return 0;
    }
    _base_weight = avg_weight; ///< 设置基础权重为平均权重
    return ResetWeight(index, 0); ///< 重置权重
}

/**
 * @brief 基于请求完成反馈更新权重
 * 
 * 根据请求的执行结果，动态调整节点权重
 * 
 * @param success 请求是否成功
 * @param begin_time_us 请求开始时间（微秒）
 * @param end_time_us 请求结束时间（微秒）
 * @param timeout_ms 请求超时时间（毫秒）
 * @param retried_count 重试次数
 * @param index 节点索引
 * @return int64_t 权重变化量
 */
int64_t LalbWeight::Update(bool success, int64_t begin_time_us, int64_t end_time_us, int64_t timeout_ms, int retried_count, size_t index) {
    int64_t latency = end_time_us - begin_time_us; ///< 计算请求延迟
    if (latency <= 0) latency = 1; ///< 确保延迟至少为1微秒

    std::lock_guard<std::mutex> lk(_mutex); ///< 加锁保护临界区
    if (_base_weight < 0) { ///< 节点已被禁用
        return 0;
    }
    _begin_time_sum -= begin_time_us; ///< 从开始时间总和中减去当前请求的开始时间
    --_begin_time_count; ///< 减少在飞请求计数
    if (_begin_time_count < 0) { ///< 确保计数非负
        _begin_time_count = 0;
        _begin_time_sum = 0;
    }

    // Match BRPC Error Penalty Logic
    if (success) {
        // Add a new entry
        TimeInfo tm_info = { latency, end_time_us };
        if (_time_q_count > 0) {
            // muduo-x ring buffer: newest is at (start + count - 1) % size
            size_t newest_idx = (_time_q_start + _time_q_count - 1) % RECV_QUEUE_SIZE;
            tm_info.latency_sum += _time_q[newest_idx].latency_sum;
        }
        
        // Push to ring buffer (elim_push equivalent)
        if (_time_q_count < RECV_QUEUE_SIZE) {
             size_t new_idx = (_time_q_start + _time_q_count) % RECV_QUEUE_SIZE;
             _time_q[new_idx] = tm_info;
             _time_q_count++;
        } else {
             // Overwrite oldest
             _time_q[_time_q_start] = tm_info;
             _time_q_start = (_time_q_start + 1) % RECV_QUEUE_SIZE;
        }
    } else {
        // Accumulate into the last entry
        // BRPC Logic: 
        // err_latency = (nleft * latency * ratio + ndone * timeout) / (ndone + nleft)
        // For simplicity in muduo-x (no max_retry in feedback), assume worst case or simple ratio
        
        int64_t err_latency = (int64_t)(latency * LalbManager::PUNISH_ERROR_RATIO);
        if (timeout_ms > 0) {
             // If we have timeout info, use it as floor or weighted average
             // BRPC uses a weighted average logic. Here we simplify but keep it robust.
             // If retried_count is available (it is in args), use it.
             // But we don't know max_retry here.
             err_latency = std::max(err_latency, timeout_ms * 1000L);
        } else {
             err_latency = std::max(err_latency, (int64_t)200000); // Default 200ms penalty
        }

        if (_time_q_count > 0) {
             size_t newest_idx = (_time_q_start + _time_q_count - 1) % RECV_QUEUE_SIZE;
             _time_q[newest_idx].latency_sum += err_latency;
             _time_q[newest_idx].end_time_us = end_time_us;
        } else {
             // First response is error
             TimeInfo tm_info = { err_latency, end_time_us };
             _time_q[_time_q_start] = tm_info;
             _time_q_count = 1;
             _time_q_start = 0; // Reset start if it was weird, though count=0 implies empty
        }
    }

    // Calculate Scaled QPS and Avg Latency (BRPC Logic)
    int64_t scaled_qps = LalbManager::WEIGHT_SCALE; // DEFAULT_QPS(1) * WEIGHT_SCALE
    
    if (_time_q_count > 1) {
        size_t oldest_idx = _time_q_start;
        size_t newest_idx = (_time_q_start + _time_q_count - 1) % RECV_QUEUE_SIZE;
        
        int64_t top_time_us = _time_q[oldest_idx].end_time_us;
        
        if (end_time_us > top_time_us) {
            if (_time_q_count == RECV_QUEUE_SIZE || 
                end_time_us >= top_time_us + 1000000L) {
                
                scaled_qps = (_time_q_count - 1) * 1000000L * LalbManager::WEIGHT_SCALE / (end_time_us - top_time_us);
                if (scaled_qps < LalbManager::WEIGHT_SCALE) {
                    scaled_qps = LalbManager::WEIGHT_SCALE;
                }
            }
            
            int64_t latency_sum_newest = _time_q[newest_idx].latency_sum;
            int64_t latency_sum_oldest = _time_q[oldest_idx].latency_sum;
            
            // Note: In BRPC, latency_sum is cumulative sum of latencies in the queue?
            // "tm_info.latency_sum += _time_q.bottom()->latency_sum;" -> Yes, it's prefix sum.
            // So avg = (sum_newest - sum_oldest) / (n - 1)
            
            _avg_latency = (latency_sum_newest - latency_sum_oldest) / (_time_q_count - 1);
        } else {
            // Time skew or too fast
            return 0;
        }
    } else if (_time_q_count == 1) {
        size_t idx = _time_q_start;
        _avg_latency = _time_q[idx].latency_sum; // For size 1, sum is just the value
    }

    if (_avg_latency <= 0) {
        return 0;
    }
    
    // BRPC: _base_weight = scaled_qps / _avg_latency;
    _base_weight = scaled_qps / _avg_latency;

    if (_base_weight < LalbManager::MIN_WEIGHT) _base_weight = LalbManager::MIN_WEIGHT; ///< 确保权重不低于最小值
    
    // 总是使用当前时间重置权重
    return ResetWeight(index, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

/**
 * @brief 设置权重值
 * 
 * @param weight 新的权重值
 */
void LalbWeight::SetWeight(int64_t weight) {
    std::lock_guard<std::mutex> lk(_mutex); ///< 加锁保护临界区
    _base_weight = weight; ///< 设置基础权重
    _weight = weight; ///< 设置当前权重
}

/**
 * @brief 禁用节点
 * 
 * @return int64_t 禁用前的权重值
 */
int64_t LalbWeight::Disable() {
    std::lock_guard<std::mutex> lk(_mutex); ///< 加锁保护临界区
    const int64_t saved = _weight; ///< 保存当前权重
    _base_weight = -1; ///< 标记为禁用
    _weight = 0; ///< 设置权重为 0
    return saved; ///< 返回保存的权重
}

/**
 * @brief 获取当前权重值
 * 
 * @return int64_t 当前权重值
 */
int64_t LalbWeight::Value() const { 
    return _weight; 
}

/**
 * @brief 获取平均延迟
 * 
 * @return int64_t 平均延迟（微秒）
 */
int64_t LalbWeight::AvgLatency() const { return _avg_latency; }

/**
 * @brief 重置权重
 * 
 * 根据当前在飞请求和延迟情况，调整权重
 * 
 * @param index 节点索引
 * @param now_us 当前时间（微秒）
 * @return int64_t 权重变化量
 */
int64_t LalbWeight::ResetWeight(size_t index, int64_t now_us) {
    int64_t new_weight = _base_weight; ///< 初始新权重为基础权重
    if (_begin_time_count > 0) { ///< 存在在飞请求
        const int64_t inflight_delay = now_us - _begin_time_sum / _begin_time_count; ///< 计算在飞请求的平均延迟
        
        // Match BRPC logic exactly:
        // const int64_t punish_latency = (int64_t)(_avg_latency * FLAGS_punish_inflight_ratio);
        // if (inflight_delay >= punish_latency && _avg_latency > 0) {
        //     new_weight = new_weight * punish_latency / inflight_delay;
        // }
        
        const int64_t punish_latency = (int64_t)(_avg_latency * LalbManager::PUNISH_INFLIGHT_RATIO);
        if (inflight_delay >= punish_latency && _avg_latency > 0) {
             new_weight = new_weight * punish_latency / inflight_delay;
        }
    }
    if (new_weight < LalbManager::MIN_WEIGHT) { ///< 确保权重不低于最小值
        new_weight = LalbManager::MIN_WEIGHT;
    }
    const int64_t old_weight = _weight; ///< 保存旧权重
    _weight = new_weight; ///< 更新当前权重
    shared_weight.store(new_weight, std::memory_order_relaxed); ///< 更新共享权重
    const int64_t diff = new_weight - old_weight; ///< 计算权重变化量
    if (_old_index == index && diff != 0) { ///< 如果是同一个节点且有权重变化
        _old_diff_sum += diff; ///< 累加权重变化量
    }
    return diff; ///< 返回权重变化量
}
