#pragma once
#include "load_balancer.h"
#include <atomic>

/**
 * @brief 加权轮询负载均衡器
 * 
 * 基于加权轮询算法实现的负载均衡器
 * 每个节点根据其权重值获得不同的请求分配比例
 * 权重越高的节点，被选中的概率越大
 */
class WeightedRoundRobinLB : public LoadBalancer {
public:
    /**
     * @brief 选择一个最佳节点
     * 
     * 实现加权轮询算法，根据节点权重分配请求
     * 支持从输入参数中提取节点权重（格式：ip:port:weight）
     * 
     * @param in 选择输入参数
     * @return std::string 选中的节点，格式为 "ip:port"
     */
    std::string select(const SelectIn& in) override;
private:
    std::atomic<size_t> index_; ///< 当前轮询索引，使用原子变量保证线程安全
};
