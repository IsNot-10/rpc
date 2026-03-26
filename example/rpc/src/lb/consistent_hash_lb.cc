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

void ConsistentHashRing::Add(const std::string& host) {
    std::vector<ConsistentHashNode> new_nodes;
    new_nodes.reserve(replica_count_);
    policy_->Build(host, replica_count_, new_nodes);
    std::sort(new_nodes.begin(), new_nodes.end());
    
    std::vector<ConsistentHashNode> merged;
    merged.reserve(nodes_.size() + new_nodes.size());
    std::merge(nodes_.begin(), nodes_.end(), 
               new_nodes.begin(), new_nodes.end(), 
               std::back_inserter(merged));
    nodes_ = std::move(merged);
}

void ConsistentHashRing::Remove(const std::string& host) {
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
        [&](const ConsistentHashNode& node) {
            return node.host == host;
        }), nodes_.end());
}

std::string ConsistentHashRing::Select(uint32_t hash) const {
    return Select(hash, [](const std::string&){ return true; });
}

std::string ConsistentHashRing::Select(uint32_t hash, const std::function<bool(const std::string&)>& predicate) const {
    if (nodes_.empty()) return "";
    
    auto it = std::lower_bound(nodes_.begin(), nodes_.end(), hash, 
        [](const ConsistentHashNode& node, uint32_t h) {
            return node.hash < h;
        });
        
    if (it == nodes_.end()) {
        it = nodes_.begin();
    }
    
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

ConsistentHashManager::ConsistentHashManager() {}

void ConsistentHashManager::EnsureHosts(const std::vector<std::string>& raw_hosts) {
    std::vector<std::string> hosts;
    hosts.reserve(raw_hosts.size());
    for (const auto& h : raw_hosts) {
        hosts.push_back(normalizeHostKey(h));
    }
    std::sort(hosts.begin(), hosts.end());
    
    std::vector<std::string> to_add;
    std::vector<std::string> to_remove;
    bool changed = false;
    
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (hosts != _last_hosts) {
            std::set_difference(hosts.begin(), hosts.end(),
                                _last_hosts.begin(), _last_hosts.end(),
                                std::back_inserter(to_add));
            std::set_difference(_last_hosts.begin(), _last_hosts.end(),
                                hosts.begin(), hosts.end(),
                                std::back_inserter(to_remove));
            _last_hosts = hosts;
            changed = true;
        }
    }
    
    if (!changed) return;
    
    _db_ring.Modify([&](ConsistentHashRing& ring) {
        if (ring.size() == 0 && !hosts.empty() && to_remove.empty()) {
             ring.Build(hosts);
        } else {
             for (const auto& h : to_remove) ring.Remove(h);
             for (const auto& h : to_add) ring.Add(h);
        }
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
        if (excluded.count(host)) {
            return false;
        }
        
        std::string hkey = normalizeHostKey(host);
        if (!ha::CircuitBreaker::instance().should_access(hkey)) {
            return false;
        }

        if (!ha::ConcurrencyLimiter::instance().is_allowed(hkey)) {
            return false;
        }
        
        return true;
    });
}

static std::unordered_map<std::string, std::unique_ptr<ConsistentHashManager>> g_ch_managers;
static std::mutex g_ch_mutex;

ConsistentHashManager& GetConsistentHashManager(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_ch_mutex);
    if (g_ch_managers.find(key) == g_ch_managers.end()) {
        g_ch_managers[key] = std::make_unique<ConsistentHashManager>();
    }
    return *g_ch_managers[key];
}

ConsistentHashLB::ConsistentHashLB(int replica_count) {
}

std::string ConsistentHashLB::select(const SelectIn& in) {
    if (in.hosts.empty()) return "";
    
    ConsistentHashManager& mgr = GetConsistentHashManager(in.service_key);
    mgr.EnsureHosts(in.hosts);
    
    std::set<std::string> excluded;
    if (in.excluded) excluded = *in.excluded;
    
    return mgr.Select(in.request_key, excluded);
}
