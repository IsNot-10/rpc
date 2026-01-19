#pragma once
#include "load_balancer.h"
#include "consistent_hash_lb.h"
#include "lalb_manager.h"
#include "weighted_round_robin_lb.h"
#include <memory>
#include <string>

// 简单工厂模式 (Simple Factory)
inline std::unique_ptr<LoadBalancer> CreateLoadBalancer(const std::string& strategy) {
    if (strategy == "consistent_hash") {
        return std::make_unique<ConsistentHashLB>();
    } else if (strategy == "lalb") {
        return std::make_unique<LalbLB>();
    } else {
        return std::make_unique<WeightedRoundRobinLB>(); // 默认使用加权轮询
    }
}
