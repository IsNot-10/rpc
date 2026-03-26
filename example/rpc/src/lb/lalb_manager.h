#pragma once
#include "lb/load_balancer.h"
#include "lb/lalb_weight.h"
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <deque>
#include <atomic>
#include <mutex>
#include "doubly_buffered_data.h"

/**
 * @brief 延迟感知负载均衡服务器信息结构体
 * 
 * 表示权重树中的一个服务器节点，包含节点的地址、权重指针和动态权重对象
 */
struct LalbServerInfo {
    std::string host;                    ///< 主机地址，格式为 "ip:port"
    std::atomic<int64_t>* left;          ///< 指向左子树权重之和的指针
    std::atomic<int64_t>* current_weight; ///< 树节点当前权重，用于保持树的一致性
    LalbWeight* weight;                  ///< 节点动态权重对象，负责权重的动态调整
};

/**
 * @brief 封装的原子变量结构体
 * 
 * 用于在权重树中存储可复制的原子变量，解决原子变量不可复制的问题
 */
struct WrappedAtomic {
    mutable std::atomic<int64_t> val;    ///< 原子变量值
    
    /**
     * @brief 构造函数
     * 
     * @param v 初始值，默认 0
     */
    WrappedAtomic(int64_t v = 0) : val(v) {}
    
    /**
     * @brief 拷贝构造函数
     * 
     * 从另一个 WrappedAtomic 对象复制值
     * 
     * @param other 另一个 WrappedAtomic 对象
     */
    WrappedAtomic(const WrappedAtomic& other) : val(other.val.load()) {}
    
    /**
     * @brief 拷贝赋值运算符
     * 
     * 从另一个 WrappedAtomic 对象复制值
     * 
     * @param other 另一个 WrappedAtomic 对象
     * @return WrappedAtomic& 当前对象的引用
     */
    WrappedAtomic& operator=(const WrappedAtomic& other) {
        val.store(other.val.load());
        return *this;
    }
};

/**
 * @brief 延迟感知负载均衡服务器集合结构体
 * 
 * 包含权重树、服务器映射和权重统计信息
 */
struct LalbServers {
    std::vector<LalbServerInfo> weight_tree;    ///< 权重树结构，用于高效选择节点
    std::unordered_map<std::string, size_t> server_map; ///< 主机到树节点索引的映射
    mutable std::vector<WrappedAtomic> left_weights; ///< 存储 left 指针指向的原子对象
    mutable std::atomic<int64_t> total;         ///< 总权重值
    
    /**
     * @brief 构造函数
     */
    LalbServers() : total(0) {}
};

/**
 * @brief 延迟感知负载均衡管理器
 * 
 * 负责管理服务器列表、维护权重树、选择最佳节点和更新节点权重
 */
class LalbManager {
public:
    /**
     * @brief 构造函数
     */
    LalbManager();
    
    /**
     * @brief 确保所有主机都在管理器中注册
     * 
     * 更新服务器列表，添加新主机，移除不存在的主机
     * 
     * @param hosts 主机列表，格式为 "ip:port"
     */
    void EnsureHosts(const std::vector<std::string>& hosts);
    
    /**
     * @brief 选择一个最佳节点
     * 
     * 基于延迟感知算法选择一个最合适的节点
     * 
     * @param excluded 需要排除的节点集合
     * @param begin_time_us 请求开始时间（微秒）
     * @return std::string 选中的节点，格式为 "ip:port"
     */
    std::string Select(const std::set<std::string>& excluded, int64_t begin_time_us);
    
    /**
     * @brief 反馈调用结果
     * 
     * 根据调用结果更新节点的权重
     * 
     * @param host 被调用的节点地址
     * @param success 调用是否成功
     * @param begin_time_us 请求开始时间（微秒）
     * @param end_time_us 请求结束时间（微秒）
     * @param timeout_ms 请求超时时间（毫秒）
     * @param retried_count 重试次数
     */
    void Feedback(const std::string& host, bool success, int64_t begin_time_us, int64_t end_time_us, int64_t timeout_ms, int retried_count);
    
    // 可配置参数
    static const int64_t WEIGHT_SCALE;         ///< 权重缩放因子
    static const int64_t MIN_WEIGHT;           ///< 最小权重值
    static const double PUNISH_INFLIGHT_RATIO; ///< 惩罚在飞请求的比例
    static const double PUNISH_ERROR_RATIO;    ///< 惩罚错误请求的比例

private:
    /**
     * @brief 更新父节点权重
     * 
     * 当某个节点的权重发生变化时，更新其父节点的权重
     * 
     * @param servers 服务器集合
     * @param diff 权重变化量
     * @param index 节点索引
     */
    static void UpdateParentWeights(const LalbServers& servers, int64_t diff, size_t index);
    
    static const size_t INITIAL_WEIGHT_TREE_SIZE = 128; ///< 初始权重树大小
    
    butil::DoublyBufferedData<LalbServers> _db_servers; ///< 双缓冲服务器集合，支持无锁读写
    std::unordered_map<std::string, LalbWeight*> _weights; ///< 持久化权重映射，跨服务器列表更新
    std::string _service_key; ///< 服务标识
    std::mutex _weights_mutex; ///< 保护_weights映射结构变更的互斥锁
};

/**
 * @brief 延迟感知负载均衡器
 * 
 * 实现 LoadBalancer 接口，使用延迟感知算法选择最佳节点
 */
class LalbLB : public LoadBalancer {
public:
    /**
     * @brief 选择一个最佳节点
     * 
     * 基于延迟感知算法选择最合适的节点
     * 
     * @param in 选择输入参数
     * @return std::string 选中的节点，格式为 "ip:port"
     */
    std::string select(const SelectIn& in) override;
    
    /**
     * @brief 反馈调用结果
     * 
     * 根据调用结果更新节点权重
     * 
     * @param info 调用反馈信息
     */
    void feedback(const CallInfo& info) override;
};

/**
 * @brief 获取延迟感知负载均衡管理器
 * 
 * 单例模式，根据服务键获取对应的管理器实例
 * 
 * @param key 服务键，格式为 ServiceName:MethodName
 * @return LalbManager& 管理器实例的引用
 */
LalbManager& GetLalbManager(const std::string& key);

/**
 * @brief 延迟感知负载均衡反馈函数
 * 
 * 向指定服务的管理器反馈调用结果
 * 
 * @param key 服务键
 * @param host 被调用的节点地址
 * @param success 调用是否成功
 * @param begin_us 请求开始时间（微秒）
 * @param end_us 请求结束时间（微秒）
 * @param timeout_ms 请求超时时间（毫秒）
 * @param retried_count 重试次数
 */
void LalbFeedback(const std::string& key, const std::string& host, bool success, int64_t begin_us, int64_t end_us, int64_t timeout_ms, int retried_count);
