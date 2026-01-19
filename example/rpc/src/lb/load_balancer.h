#pragma once
#include <string>
#include <vector>
#include <set>

struct SelectIn {
    std::vector<std::string> hosts;          // 候选主机列表 (ip:port)
    std::string service_key;                 // 服务标识 (ServiceName:MethodName)
    std::string request_key;                 // 请求标识 (用于一致性哈希等)
    const std::set<std::string>* excluded = nullptr; // 排除节点集合 (用于重试/熔断)
    int64_t begin_time_us = 0;               // 请求开始时间 (微秒)
};

struct CallInfo {
    std::string service_key;                 // 服务标识
    std::string host;                        // 实际调用的主机
    bool success;                            // 是否调用成功
    int64_t begin_time_us;                   // 开始时间 (微秒)
    int64_t end_time_us;                     // 结束时间 (微秒)
    int64_t timeout_ms;                      // 设定的超时时间 (毫秒)
    int retried_count;                       // 第几次重试 (0表示首次)
};

class LoadBalancer {
public:
    virtual ~LoadBalancer() = default;
    
    // 选择一个最佳节点
    virtual std::string select(const SelectIn& in) = 0;
    
    // 调用反馈 (用于更新负载均衡状态，如 LALB)
    virtual void feedback(const CallInfo& info) {}
};
