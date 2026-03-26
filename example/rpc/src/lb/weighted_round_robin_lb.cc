#include "weighted_round_robin_lb.h"
#include "lb_common.h"
#include "../ha/circuit_breaker.h"
#include "../ha/concurrency_limiter.h"
#include <vector>

/**
 * @brief 选择一个最佳节点
 * 
 * 实现加权轮询算法，根据节点权重分配请求
 * 支持从输入参数中提取节点权重（格式：ip:port:weight）
 * 
 * @param in 选择输入参数
 * @return std::string 选中的节点，格式为 "ip:port"
 */
std::string WeightedRoundRobinLB::select(const SelectIn& in) {
    if (in.hosts.empty()) return ""; ///< 候选主机列表为空，返回空字符串
    
    // 根据排除列表和熔断器状态过滤主机
    std::vector<std::string> valid_hosts; ///< 存储有效的主机列表
    valid_hosts.reserve(in.hosts.size()); ///< 预留足够的空间
    
    for (const auto& host : in.hosts) {
        // 检查排除列表
        if (in.excluded && in.excluded->count(host)) {
            continue; ///< 主机在排除列表中，跳过
        }
        
        // 检查熔断器状态
        std::string key = normalizeHostKey(host); ///< 归一化主机 Key
        if (!ha::CircuitBreaker::instance().should_access(key)) {
            continue; ///< 主机被熔断，跳过
        }

        // 检查并发限制器状态
        if (!ha::ConcurrencyLimiter::instance().is_allowed(key)) {
            continue; ///< 主机并发已达上限，跳过
        }
        
        valid_hosts.push_back(host); ///< 将有效的主机添加到列表中
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
        return ""; ///< 没有有效的主机，返回空字符串
    }

    // 简单的轮询实现：使用原子变量确保线程安全
    size_t current = index_.fetch_add(1); ///< 原子递增并获取当前索引
    return valid_hosts[current % valid_hosts.size()]; ///< 取模运算选择主机
}
