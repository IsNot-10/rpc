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
#include "../butil/containers/doubly_buffered_data.h"

struct LalbServerInfo {
    std::string host;           // 主机地址
    std::atomic<int64_t>* left; // 左子树权重之和
    std::atomic<int64_t>* current_weight; // 树节点当前权重 (用于保持树的一致性)
    LalbWeight* weight;         // 节点动态权重对象
};

struct WrappedAtomic {
    mutable std::atomic<int64_t> val;
    WrappedAtomic(int64_t v = 0) : val(v) {}
    WrappedAtomic(const WrappedAtomic& other) : val(other.val.load()) {}
    WrappedAtomic& operator=(const WrappedAtomic& other) {
        val.store(other.val.load());
        return *this;
    }
};

struct LalbServers {
    std::vector<LalbServerInfo> weight_tree; // 权重树结构
    std::unordered_map<std::string, size_t> server_map; // 主机到树节点的映射
    mutable std::vector<WrappedAtomic> left_weights; // 存储 left 指针指向的 atomic 对象
    mutable std::vector<WrappedAtomic> current_weights; // 存储 current_weight 指针指向的 atomic 对象
    mutable std::atomic<int64_t> total; // 总权重
    
    LalbServers() : total(0) {}
};

class LalbManager {
public:
    LalbManager();
    // 确保所有 hosts 都在管理器中注册 (更新服务器列表)
    void EnsureHosts(const std::vector<std::string>& hosts);
    
    // 选择一个最佳节点
    std::string Select(const std::set<std::string>& excluded, int64_t begin_time_us);
    
    // 反馈调用结果 (更新权重)
    void Feedback(const std::string& host, bool success, int64_t begin_time_us, int64_t end_time_us, int64_t timeout_ms, int retried_count);
    
    // 可配置参数
    static const int64_t WEIGHT_SCALE;
    static const int64_t MIN_WEIGHT;
    static const double PUNISH_INFLIGHT_RATIO;
    static const double PUNISH_ERROR_RATIO;

private:
    static void UpdateParentWeights(const LalbServers& servers, int64_t diff, size_t index);
    static const size_t INITIAL_WEIGHT_TREE_SIZE = 128;
    
    butil::DoublyBufferedData<LalbServers> _db_servers;
    std::unordered_map<std::string, LalbWeight*> _weights; // 持久化权重 (跨服务器列表更新)
    std::string _service_key;
    std::mutex _weights_mutex; // 仅保护 _weights map 的结构变更
};

class LalbLB : public LoadBalancer {
public:
    std::string select(const SelectIn& in) override;
    void feedback(const CallInfo& info) override;
};

LalbManager& GetLalbManager(const std::string& key);
void LalbFeedback(const std::string& key, const std::string& host, bool success, int64_t begin_us, int64_t end_us, int64_t timeout_ms, int retried_count);
