#pragma once
#include "load_balancer.h"
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <algorithm>
#include <map>
#include <functional>

/**
 * @brief 一致性哈希环的虚拟节点结构
 * 
 * 代表一致性哈希环上的一个虚拟节点，包含哈希值和对应的物理节点地址
 */
struct ConsistentHashNode {
    uint32_t hash;        ///< 虚拟节点的哈希值
    std::string host;     ///< 对应的物理节点地址，格式为 "ip:port"
    
    /**
     * @brief 排序运算符
     * 
     * 排序规则：
     * 1. 首先按 Hash 值从小到大排序
     * 2. 若 Hash 值冲突，则按 Host 字典序排序（保证确定性）
     * 
     * @param other 另一个虚拟节点
     * @return bool 当前节点是否小于另一个节点
     */
    bool operator<(const ConsistentHashNode& other) const {
        if (hash != other.hash) {
            return hash < other.hash;
        }
        return host < other.host;
    }
};

/**
 * @brief 抽象副本策略（虚拟节点生成策略）
 * 
 * 定义了如何为物理节点生成虚拟节点的接口
 */
class ReplicaPolicy {
public:
    /**
     * @brief 虚析构函数
     */
    virtual ~ReplicaPolicy() = default;
    
    /**
     * @brief 为物理节点生成虚拟节点
     * 
     * 纯虚函数，必须由子类实现
     * 
     * @param host 物理节点地址，格式为 "ip:port"
     * @param replica_count 需要生成的虚拟节点数量
     * @param nodes 输出参数，存储生成的虚拟节点
     */
    virtual void Build(const std::string& host, int replica_count, std::vector<ConsistentHashNode>& nodes) const = 0;
};

/**
 * @brief 默认副本策略
 * 
 * 使用 MurmurHash3 算法生成虚拟节点
 */
class DefaultReplicaPolicy : public ReplicaPolicy {
public:
    /**
     * @brief 为物理节点生成虚拟节点
     * 
     * 使用 MurmurHash3 算法为物理节点生成指定数量的虚拟节点
     * 
     * @param host 物理节点地址，格式为 "ip:port"
     * @param replica_count 需要生成的虚拟节点数量
     * @param nodes 输出参数，存储生成的虚拟节点
     */
    void Build(const std::string& host, int replica_count, std::vector<ConsistentHashNode>& nodes) const override;
};

#include "doubly_buffered_data.h"

/**
 * @brief 一致性哈希环
 * 
 * 管理一致性哈希算法的核心数据结构，负责虚拟节点的构建和节点选择
 */
class ConsistentHashRing {
public:
    /**
     * @brief 构造函数
     * 
     * @param replica_count 每个物理节点生成的虚拟节点数量，默认 100
     */
    explicit ConsistentHashRing(int replica_count = 100);
    
    /**
     * @brief 构建哈希环
     * 
     * 根据提供的物理节点列表，重新构建整个哈希环
     * 时间复杂度 O(N)
     * 
     * @param hosts 物理节点列表，格式为 "ip:port"
     */
    void Build(const std::vector<std::string>& hosts);
    
    /**
     * @brief 添加物理节点
     * 
     * 向哈希环中添加一个物理节点及其对应的虚拟节点
     * 
     * @param host 物理节点地址，格式为 "ip:port"
     */
    void Add(const std::string& host);
    
    /**
     * @brief 移除物理节点
     * 
     * 从哈希环中移除一个物理节点及其对应的虚拟节点
     * 
     * @param host 物理节点地址，格式为 "ip:port"
     */
    void Remove(const std::string& host);
    
    /**
     * @brief 根据哈希值选择节点
     * 
     * 找到哈希环上第一个哈希值大于等于给定哈希值的虚拟节点
     * 并返回其对应的物理节点地址
     * 
     * @param hash 输入的哈希值
     * @return std::string 选中的物理节点地址，格式为 "ip:port"
     */
    std::string Select(uint32_t hash) const;
    
    /**
     * @brief 根据哈希值和谓词选择节点
     * 
     * 找到哈希环上第一个满足谓词条件的虚拟节点
     * 
     * @param hash 输入的哈希值
     * @param predicate 谓词函数，用于过滤节点
     * @return std::string 选中的物理节点地址，格式为 "ip:port"
     */
    std::string Select(uint32_t hash, const std::function<bool(const std::string&)>& predicate) const;
    
    /**
     * @brief 获取虚拟节点数量
     * 
     * @return size_t 虚拟节点的数量
     */
    size_t size() const { return nodes_.size(); }
    
private:
    int replica_count_;                  ///< 每个物理节点生成的虚拟节点数量
    std::vector<ConsistentHashNode> nodes_; ///< 排序后的虚拟节点列表
    std::unique_ptr<ReplicaPolicy> policy_; ///< 虚拟节点生成策略
};

/**
 * @brief 一致性哈希管理器
 * 
 * 管理一致性哈希环的生命周期，支持动态更新和线程安全访问
 */
class ConsistentHashManager {
public:
    /**
     * @brief 构造函数
     */
    ConsistentHashManager();
    
    /**
     * @brief 确保哈希环包含指定的物理节点
     * 
     * 动态更新哈希环，添加新节点或移除不存在的节点
     * 
     * @param hosts 物理节点列表，格式为 "ip:port"
     */
    void EnsureHosts(const std::vector<std::string>& hosts);
    
    /**
     * @brief 根据键选择节点
     * 
     * 根据键的哈希值选择节点，同时排除指定的节点
     * 
     * @param key 用于生成哈希值的键
     * @param excluded 需要排除的节点集合
     * @return std::string 选中的物理节点地址，格式为 "ip:port"
     */
    std::string Select(const std::string& key, const std::set<std::string>& excluded);
    
private:
    butil::DoublyBufferedData<ConsistentHashRing> _db_ring; ///< 双缓冲哈希环，支持无锁读写
    std::mutex _mutex;                                      ///< 保护_last_hosts的互斥锁
    std::vector<std::string> _last_hosts;                   ///< 上一次的物理节点列表
};

/**
 * @brief 一致性哈希负载均衡器
 * 
 * 基于一致性哈希算法实现的负载均衡器，继承自 LoadBalancer 接口
 */
class ConsistentHashLB : public LoadBalancer {
public:
    /**
     * @brief 构造函数
     * 
     * @param replica_count 每个物理节点生成的虚拟节点数量，默认 160
     */
    explicit ConsistentHashLB(int replica_count = 160);
    
    /**
     * @brief 选择一个最佳节点
     * 
     * 基于一致性哈希算法，根据输入参数选择一个最合适的节点
     * 
     * @param in 选择输入参数
     * @return std::string 选中的节点，格式为 "ip:port"
     */
    std::string select(const SelectIn& in) override;
    
    /**
     * @brief 设置哈希环（已废弃）
     * 
     * 哈希环现在由内部管理，此方法仅为兼容旧代码保留
     * 
     * @param ring 哈希环指针（未使用）
     */
    void SetRing(std::shared_ptr<ConsistentHashRing> ring) {}
    
private:
    // 无状态设计，所有状态由 ConsistentHashManager 管理
};
