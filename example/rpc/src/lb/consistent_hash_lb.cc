#include "consistent_hash_lb.h"
#include "../MurmurHash3.h"
#include "lb_common.h"
#include "../ha/circuit_breaker.h"
#include "../ha/concurrency_limiter.h"
#include <iostream>
#include <functional>
#include <algorithm>

static uint32_t getHash(const std::string& key) {
    uint32_t out;
    MurmurHash3_x86_32(key.c_str(), key.length(), 0x12345678, &out);
    return out;
}

void DefaultReplicaPolicy::Build(const std::string& host, int replica_count, std::vector<ConsistentHashNode>& nodes) const {
    for (int i = 0; i < replica_count; ++i) {
        // BRPC 风格：host + "-" + i
        std::string virtual_node_key = host + "-" + std::to_string(i);
        ConsistentHashNode node;
        node.hash = getHash(virtual_node_key);
        node.host = host;
        nodes.push_back(node);
    }
}

ConsistentHashRing::ConsistentHashRing(int replica_count) 
    : replica_count_(replica_count) {
    policy_ = std::make_unique<DefaultReplicaPolicy>();
}

void ConsistentHashRing::Build(const std::vector<std::string>& hosts) {
    nodes_.clear();
    if (hosts.empty()) return;
    nodes_.reserve(hosts.size() * replica_count_);
    
    for (const auto& host : hosts) {
        policy_->Build(host, replica_count_, nodes_);
    }
    
    std::sort(nodes_.begin(), nodes_.end());
}

std::string ConsistentHashRing::Select(uint32_t hash) const {
    return Select(hash, [](const std::string&){ return true; });
}

std::string ConsistentHashRing::Select(uint32_t hash, const std::function<bool(const std::string&)>& predicate) const {
    if (nodes_.empty()) return "";
    
    // 下界查找：找到第一个 hash 值 >= 目标 hash 的节点
    auto it = std::lower_bound(nodes_.begin(), nodes_.end(), hash, 
        [](const ConsistentHashNode& node, uint32_t h) {
            return node.hash < h;
        });
        
    // 环形回绕：如果超过末尾，则回到开头
    if (it == nodes_.end()) {
        it = nodes_.begin();
    }
    
    // 遍历环以找到一个有效节点 (满足 predicate)
    // 为避免死循环，我们需要记录起始位置
    auto start_it = it;
    
    do {
        if (predicate(it->host)) {
            return it->host;
        }
        
        ++it;
        if (it == nodes_.end()) {
            it = nodes_.begin();
        }
    } while (it != start_it);
    
    return "";
}

// --- ConsistentHashManager ---

ConsistentHashManager::ConsistentHashManager() {}

void ConsistentHashManager::EnsureHosts(const std::vector<std::string>& raw_hosts) {
    // 1. Normalize and sort hosts for comparison
    std::vector<std::string> hosts;
    hosts.reserve(raw_hosts.size());
    for (const auto& h : raw_hosts) {
        hosts.push_back(normalizeHostKey(h));
    }
    std::sort(hosts.begin(), hosts.end());
    
    // 2. Check if changed (optimistic lock-free check? No, we need mutex for _last_hosts)
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (hosts != _last_hosts) {
            _last_hosts = hosts;
            changed = true;
        }
    }
    
    if (!changed) return;
    
    // 3. Update Ring using Modify
    _db_ring.Modify([&hosts](ConsistentHashRing& ring) {
        ring.Build(hosts);
        return 1;
    });
}

std::string ConsistentHashManager::Select(const std::string& key, const std::set<std::string>& excluded) {
    butil::DoublyBufferedData<ConsistentHashRing>::ScopedPtr ring;
    if (_db_ring.Read(&ring) != 0) {
        return "";
    }
    
    uint32_t hash = getHash(key);
    
    return ring->Select(hash, [&](const std::string& host) {
        // 检查排除列表
        if (excluded.count(host)) {
            return false;
        }
        
        // 检查熔断器状态
        std::string hkey = normalizeHostKey(host);
        if (!ha::CircuitBreaker::instance().should_access(hkey)) {
            return false;
        }

        // 检查并发限制器状态
        if (!ha::ConcurrencyLimiter::instance().is_allowed(hkey)) {
            return false;
        }
        
        return true;
    });
}

// --- Global Manager Access ---

static std::unordered_map<std::string, std::unique_ptr<ConsistentHashManager>> g_ch_managers;
static std::mutex g_ch_mutex;

ConsistentHashManager& GetConsistentHashManager(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_ch_mutex);
    if (g_ch_managers.find(key) == g_ch_managers.end()) {
        g_ch_managers[key] = std::make_unique<ConsistentHashManager>();
    }
    return *g_ch_managers[key];
}

// --- ConsistentHashLB ---

ConsistentHashLB::ConsistentHashLB(int replica_count) {
    // Replica count is now managed by Ring (default 100) or we could pass it to Manager if needed.
    // For now, we ignore it or use it to initialize Manager (but Manager is shared).
}

std::string ConsistentHashLB::select(const SelectIn& in) {
    if (in.hosts.empty()) return "";
    
    ConsistentHashManager& mgr = GetConsistentHashManager(in.service_key);
    mgr.EnsureHosts(in.hosts);
    
    std::set<std::string> excluded;
    if (in.excluded) excluded = *in.excluded;
    
    return mgr.Select(in.request_key, excluded);
}
