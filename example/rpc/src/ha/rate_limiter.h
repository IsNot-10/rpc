#pragma once
#include <mutex>
#include <chrono>

namespace ha {

/**
 * @brief 简单的令牌桶限流器
 * 
 * 基于令牌桶算法实现的速率限制器
 * 作用：限制发送到某个服务的总体 QPS，防止服务过载
 * 
 * 令牌桶算法原理：
 * 1. 系统以固定速率向桶中添加令牌
 * 2. 当请求到达时，尝试从桶中获取令牌
 * 3. 若获取成功，则允许请求通过
 * 4. 若获取失败，则拒绝请求（限流）
 */
class RateLimiter {
public:
    /**
     * @brief 构造函数
     * 
     * @param qps 每秒生成的令牌数，即 QPS 限制
     * @param max_burst 桶容量，即最大突发量
     */
    RateLimiter(double qps, double max_burst) 
        : qps_(qps),              ///< 每秒生成的令牌数
          max_burst_(max_burst),  ///< 桶容量，最大突发量
          tokens_(max_burst)      ///< 初始令牌数，等于桶容量
    {
        last_refill_ = std::chrono::steady_clock::now(); ///< 初始化上次补充时间为当前时间
    }
    
    /**
     * @brief 尝试获取令牌
     * 
     * @param tokens 需要消耗的令牌数，默认 1.0
     * @return bool 获取成功返回 true（允许通过），失败返回 false（限流）
     */
    bool try_acquire(double tokens = 1.0) {
        std::lock_guard<std::mutex> lock(mutex_); ///< 加锁保护临界区
        refill(); ///< 先补充令牌
        if (tokens_ >= tokens) { ///< 检查剩余令牌是否足够
            tokens_ -= tokens; ///< 消耗令牌
            return true; ///< 获取成功
        }
        return false; ///< 令牌不足，获取失败
    }

private:
    /**
     * @brief 补充令牌
     * 
     * 根据时间流逝，按照固定速率向桶中添加令牌
     */
    void refill() {
        auto now = std::chrono::steady_clock::now(); ///< 获取当前时间
        
        // 计算自上次补充以来经过的秒数
        double elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last_refill_).count() / 1000000.0;
        
        // 根据经过的时间和速率计算新增令牌数
        tokens_ += elapsed * qps_;
        
        // 确保令牌数不超过桶容量
        if (tokens_ > max_burst_) tokens_ = max_burst_;
        
        // 更新上次补充时间
        last_refill_ = now;
    }

    double qps_;            ///< 每秒生成的令牌数 (QPS 限制)
    double max_burst_;      ///< 桶容量 (最大突发量)
    double tokens_;         ///< 当前剩余令牌数
    std::chrono::steady_clock::time_point last_refill_; ///< 上次补充令牌的时间点
    std::mutex mutex_;      ///< 保护临界区的互斥锁
};

} // namespace ha
