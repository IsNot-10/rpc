#include "weighted_round_robin_lb.h"
#include "lb_common.h"
#include "../ha/circuit_breaker.h"
#include "../ha/concurrency_limiter.h"
#include <vector>

std::string WeightedRoundRobinLB::select(const SelectIn& in) {
    if (in.hosts.empty()) return "";
    
    // 根据排除列表和熔断器状态过滤主机
    std::vector<std::string> valid_hosts;
    valid_hosts.reserve(in.hosts.size());
    
    for (const auto& host : in.hosts) {
        // 检查排除列表
        if (in.excluded && in.excluded->count(host)) {
            continue;
        }
        
        // 检查熔断器状态
        std::string key = normalizeHostKey(host);
        if (!ha::CircuitBreaker::instance().should_access(key)) {
            continue;
        }

        // 检查并发限制器状态
        if (!ha::ConcurrencyLimiter::instance().is_allowed(key)) {
            continue;
        }
        
        valid_hosts.push_back(host);
    }
    
    // 降级策略：如果所有主机都被排除或熔断
    // 1. 返回空 -> 请求立即失败
    // 2. 从原始列表中返回 -> 尝试请求 (可能是探测?)
    // BRPC 通常会在所有过滤后返回错误
    // 但是，如果 valid_hosts 为空是因为排除列表 (重试机制)，
    // 我们是否应该尝试原始主机? 
    // 但 'excluded' 意味着 "本次请求中已尝试并失败"，所以不应重试
    // 如果是 'broken' (熔断)，则不应访问
    
    if (valid_hosts.empty()) {
        return "";
    }

    static std::atomic<size_t> index{0};
    size_t current = index.fetch_add(1);
    return valid_hosts[current % valid_hosts.size()];
}
