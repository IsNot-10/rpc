#pragma once
#include <string>
#include <vector>
#include <set>

/**
 * @brief 负载均衡选择输入参数结构体
 * 
 * 包含选择节点时所需的所有输入信息
 */
struct SelectIn {
    std::vector<std::string> hosts;          ///< 候选主机列表，格式为 ip:port
    std::string service_key;                 ///< 服务标识，格式为 ServiceName:MethodName
    std::string request_key;                 ///< 请求标识，用于一致性哈希等算法
    const std::set<std::string>* excluded = nullptr; ///< 排除节点集合，用于重试或熔断场景
    int64_t begin_time_us = 0;               ///< 请求开始时间，单位为微秒
};

/**
 * @brief 调用反馈信息结构体
 * 
 * 包含调用结束后需要反馈给负载均衡器的信息
 */
struct CallInfo {
    std::string service_key;                 ///< 服务标识，格式为 ServiceName:MethodName
    std::string host;                        ///< 实际调用的主机，格式为 ip:port
    bool success;                            ///< 调用是否成功
    int64_t begin_time_us;                   ///< 调用开始时间，单位为微秒
    int64_t end_time_us;                     ///< 调用结束时间，单位为微秒
    int64_t timeout_ms;                      ///< 设定的超时时间，单位为毫秒
    int retried_count;                       ///< 重试次数，0表示首次调用
};

/**
 * @brief 负载均衡器抽象基类
 * 
 * 定义了负载均衡算法的通用接口，所有具体负载均衡算法必须继承并实现此接口
 */
class LoadBalancer {
public:
    /**
     * @brief 虚析构函数
     * 
     * 确保子类能够正确释放资源
     */
    virtual ~LoadBalancer() = default;
    
    /**
     * @brief 选择一个最佳节点
     * 
     * 根据输入参数选择一个最合适的节点进行调用
     * 纯虚函数，必须由子类实现
     * 
     * @param in 选择输入参数
     * @return std::string 选中的节点，格式为 ip:port
     */
    virtual std::string select(const SelectIn& in) = 0;
    
    /**
     * @brief 调用反馈
     * 
     * 用于更新负载均衡器的状态，例如记录节点的响应时间、成功率等
     * 可选实现，默认空操作
     * 
     * @param info 调用反馈信息
     */
    virtual void feedback(const CallInfo& info) {}
};
