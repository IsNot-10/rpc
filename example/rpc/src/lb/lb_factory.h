#pragma once
#include "load_balancer.h"
#include "consistent_hash_lb.h"
#include "lalb_manager.h"
#include "weighted_round_robin_lb.h"
#include <memory>
#include <string>

/**
 * @brief 负载均衡器工厂函数
 * 
 * 基于简单工厂模式，根据指定的策略创建相应的负载均衡器实例
 * 
 * @param strategy 负载均衡策略名称
 * @return std::shared_ptr<LoadBalancer> 创建的负载均衡器智能指针
 * 
 * 支持的策略：
 * - "consistent_hash" : 一致性哈希负载均衡
 * - "lalb" : 延迟感知负载均衡
 * - 默认 : 加权轮询负载均衡
 */
inline std::shared_ptr<LoadBalancer> CreateLoadBalancer(const std::string& strategy) {
    if (strategy == "consistent_hash") {
        return std::make_shared<ConsistentHashLB>();  ///< 创建一致性哈希负载均衡器
    } else if (strategy == "lalb") {
        return std::make_shared<LalbLB>();           ///< 创建延迟感知负载均衡器
    } else {
        return std::make_shared<WeightedRoundRobinLB>(); ///< 默认创建加权轮询负载均衡器
    }
}
