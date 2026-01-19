#pragma once
#include "load_balancer.h"
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <algorithm>
#include <map>
#include <functional>

// 一致性哈希环的节点结构
struct ConsistentHashNode {
    uint32_t hash;        // 虚拟节点的哈希值
    std::string host;     // 物理节点地址 "ip:port"
    
    // 排序规则：按 Hash 值排序，若 Hash 冲突则按 Host 字典序 (保证确定性)
    bool operator<(const ConsistentHashNode& other) const {
        if (hash != other.hash) {
            return hash < other.hash;
        }
        return host < other.host;
    }
};

// 抽象副本策略 (虚拟节点生成策略)
class ReplicaPolicy {
public:
    virtual ~ReplicaPolicy() = default;
    // 生成虚拟节点
    virtual void Build(const std::string& host, int replica_count, std::vector<ConsistentHashNode>& nodes) const = 0;
};

// 默认策略 (使用 MurmurHash3)
class DefaultReplicaPolicy : public ReplicaPolicy {
public:
    void Build(const std::string& host, int replica_count, std::vector<ConsistentHashNode>& nodes) const override;
};

#include "../butil/containers/doubly_buffered_data.h"

// 哈希环状态 (线程安全读，写操作通常通过替换整个 Ring 对象来实现)
class ConsistentHashRing {
public:
    ConsistentHashRing(int replica_count = 100);
    // 支持移动构造
    ConsistentHashRing(ConsistentHashRing&&) = default;
    ConsistentHashRing& operator=(ConsistentHashRing&&) = default;
    
    // 构建哈希环
    void Build(const std::vector<std::string>& hosts);
    
    // 选择满足条件的节点 (Predicate 用于过滤被熔断或排除的节点)
    // 如果找不到满足条件的节点，返回空字符串
    std::string Select(uint32_t hash, const std::function<bool(const std::string&)>& predicate) const;
    
    // 简单选择 (无过滤)
    std::string Select(uint32_t hash) const;

    size_t size() const { return nodes_.size(); }
    
private:
    int replica_count_;                    // 每个物理节点的虚拟节点数
    std::vector<ConsistentHashNode> nodes_; // 哈希环上的所有节点 (有序)
    std::unique_ptr<ReplicaPolicy> policy_; // 虚拟节点生成策略
    
    // 禁止拷贝，因为 unique_ptr
    ConsistentHashRing(const ConsistentHashRing&) = delete;
    ConsistentHashRing& operator=(const ConsistentHashRing&) = delete;
};

class ConsistentHashManager {
public:
    ConsistentHashManager();
    void EnsureHosts(const std::vector<std::string>& hosts);
    std::string Select(const std::string& key, const std::set<std::string>& excluded);
    
private:
    butil::DoublyBufferedData<ConsistentHashRing> _db_ring;
    std::vector<std::string> _last_hosts; // 用于检查变更
    std::mutex _mutex;
};

class ConsistentHashLB : public LoadBalancer {
public:
    explicit ConsistentHashLB(int replica_count = 160);
    
    std::string select(const SelectIn& in) override;
    
    // Deprecated: Ring is managed internally now
    void SetRing(std::shared_ptr<ConsistentHashRing> ring) {}
    
private:
    // No state here
};
