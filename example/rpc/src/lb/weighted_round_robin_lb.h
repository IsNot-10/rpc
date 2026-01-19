#pragma once
#include "load_balancer.h"
#include <atomic>

class WeightedRoundRobinLB : public LoadBalancer {
public:
    // 选择节点：简单的轮询 (Round Robin) 实现
    // TODO: 目前实现是简单的 RR，后续支持权重 (Weighted)
    std::string select(const SelectIn& in) override;
};
