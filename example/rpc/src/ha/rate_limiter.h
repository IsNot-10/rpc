#pragma once
#include <mutex>
#include <chrono>

namespace ha {

// 简单的令牌桶限流器 (Simple Token Bucket Rate Limiter)
// 作用：限制发送到某个服务的总体 QPS，防止过载
class RateLimiter {
public:
    RateLimiter(double qps, double max_burst) 
        : qps_(qps), max_burst_(max_burst), tokens_(max_burst) {
        last_refill_ = std::chrono::steady_clock::now();
    }
    
    // 尝试获取令牌 (Try Acquire)
    // tokens: 需要消耗的令牌数
    // 返回: true 表示获取成功 (允许通过)，false 表示获取失败 (限流)
    bool try_acquire(double tokens = 1.0) {
        std::lock_guard<std::mutex> lock(mutex_);
        refill();
        if (tokens_ >= tokens) {
            tokens_ -= tokens;
            return true;
        }
        return false;
    }

private:
    // 补充令牌 (Refill)
    void refill() {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last_refill_).count() / 1000000.0;
        tokens_ += elapsed * qps_;
        if (tokens_ > max_burst_) tokens_ = max_burst_;
        last_refill_ = now;
    }

    double qps_;            // 每秒生成的令牌数 (QPS 限制)
    double max_burst_;      // 桶容量 (最大突发量)
    double tokens_;         // 当前剩余令牌数
    std::chrono::steady_clock::time_point last_refill_; // 上次补充时间
    std::mutex mutex_;
};

} // namespace ha
