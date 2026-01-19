#include "lb/lalb_manager.h"
#include "lb/lb_common.h"
#include "Logging.h"
#include "../ha/circuit_breaker.h"
#include "../ha/concurrency_limiter.h"
#include <cmath>
#include <numeric>
#include <algorithm>
#include <random>

const int64_t LalbManager::WEIGHT_SCALE = 1000000; // 增加比例以提高微秒级精度
const int64_t LalbManager::MIN_WEIGHT = 1; 
const double LalbManager::PUNISH_INFLIGHT_RATIO = 1.5;
const double LalbManager::PUNISH_ERROR_RATIO = 100.0; // 错误惩罚系数

LalbManager::LalbManager() {}

void LalbManager::UpdateParentWeights(const LalbServers& servers, int64_t diff, size_t index) {
    // 1. 更新当前节点的 current_weight
    if (index < servers.current_weights.size()) {
        servers.current_weights[index].val.fetch_add(diff);
    }
    
    // 2. 向上更新父节点的 left sum
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (parent * 2 + 1 == index) { // 如果是左子节点，父节点的 left 需要增加
            if (parent < servers.left_weights.size()) {
                servers.left_weights[parent].val.fetch_add(diff);
            }
        }
        // 如果是右子节点，父节点的 left 不需要改变
        index = parent;
    }
}

void LalbManager::EnsureHosts(const std::vector<std::string>& raw_hosts) {
    // 归一化主机列表
    std::vector<std::string> hosts;
    hosts.reserve(raw_hosts.size());
    for (const auto& h : raw_hosts) {
         std::string key = normalizeHostKey(h);
         hosts.push_back(key);
    }
    
    // 准备权重对象 (需要在 Modify 之外进行，以减少锁竞争)
    std::vector<LalbWeight*> host_weights;
    host_weights.reserve(hosts.size());
    
    {
        std::lock_guard<std::mutex> lock(_weights_mutex);
        for (const auto& host : hosts) {
            if (_weights.find(host) == _weights.end()) {
                _weights[host] = new LalbWeight(WEIGHT_SCALE); 
            }
            host_weights.push_back(_weights[host]);
        }
    }

    auto update_fn = [&](LalbServers& servers) {
        // 检查是否需要更新 (简单比较数量，更严谨应该比较内容)
        // 由于 Modify 会被调用两次，且我们已经在外面准备好了 host_weights，
        // 这里直接重建是安全的。
        
        servers.server_map.clear();
        servers.weight_tree.clear();
        servers.left_weights.clear();
        servers.current_weights.clear();
        
        // 预分配
        servers.weight_tree.reserve(hosts.size());
        servers.left_weights.reserve(hosts.size());
        servers.current_weights.reserve(hosts.size());
        
        // 初始化节点
        for (size_t i = 0; i < hosts.size(); ++i) {
            const auto& host = hosts[i];
            LalbWeight* w = host_weights[i];
            
            servers.left_weights.emplace_back(0);
            servers.current_weights.emplace_back(w->Value()); 
            
            LalbServerInfo info;
            info.host = host;
            info.weight = w;
            // 指针指向 vector 中的元素
            // 注意：vector 扩容会导致指针失效，但我们已经 reserve 了，且之后不再 push_back
            // 为了安全起见，我们在循环结束后再设置指针？
            // 不，servers.current_weights[i] 是引用 WrappedAtomic，取地址 &servers.current_weights[i].val
            // 由于 vector 已经 reserve，且我们是一次性填满，不会再扩容。
            // 但是为了绝对安全，最好用索引访问？
            // 之前的代码是用指针。
            // 这里我们先存下来，最后再修复指针？
            // 或者直接用 vector 索引访问？
            // UpdateParentWeights 使用索引。
            // Select 使用指针。
            // 这里我们先 push_back，最后统一设置指针。
            servers.weight_tree.push_back(info);
            servers.server_map[host] = i;
        }
        
        // 修复指针 (vector 填充完毕，地址稳定)
        for (size_t i = 0; i < hosts.size(); ++i) {
            servers.weight_tree[i].left = &servers.left_weights[i].val;
            servers.weight_tree[i].current_weight = &servers.current_weights[i].val;
        }
        
        // 构建树并计算 subtree weights (Bottom-up)
        size_t n = servers.weight_tree.size();
        if (n > 0) {
            std::vector<int64_t> subtree_weights(n, 0);
            for (int i = n - 1; i >= 0; --i) {
                int64_t w = servers.current_weights[i].val.load();
                size_t l_idx = 2 * i + 1;
                size_t r_idx = 2 * i + 2;
                
                int64_t l_sum = (l_idx < n) ? subtree_weights[l_idx] : 0;
                int64_t r_sum = (r_idx < n) ? subtree_weights[r_idx] : 0;
                
                servers.left_weights[i].val.store(l_sum);
                subtree_weights[i] = w + l_sum + r_sum;
            }
            servers.total.store(subtree_weights[0]);
        } else {
            servers.total.store(0);
        }
        return 1;
    };
    
    _db_servers.Modify(update_fn);
    
    // 简单的日志，无法获取 total_weight 除非从 servers 读取
    LOG_INFO << "LALB updated hosts, count=" << hosts.size();
}

std::string LalbManager::Select(const std::set<std::string>& excluded, int64_t begin_time_us) {
    butil::DoublyBufferedData<LalbServers>::ScopedPtr servers;
    if (_db_servers.Read(&servers) != 0) {
        return "";
    }
    
    if (servers->weight_tree.empty()) return "";
    
    int64_t total = servers->total.load();
    if (total <= 0) return "";
    
    static thread_local std::mt19937 generator(std::random_device{}());
    
    size_t nloop = 0;
    std::string last_candidate_host;
    
    while (total > 0) {
        if (++nloop > 3) {
             if (!last_candidate_host.empty()) return last_candidate_host;
             break;
        }
        
        std::uniform_int_distribution<int64_t> distribution(0, total - 1);
        int64_t dice = distribution(generator);
        size_t index = 0;
        size_t n = servers->weight_tree.size();
        
        bool found = false;
        while (index < n) {
            const auto& info = servers->weight_tree[index];
            int64_t left = info.left->load();
            int64_t self = info.current_weight->load();
            
            if (dice < left) {
                index = index * 2 + 1;
            } else if (dice < left + self) {
                // 选中当前节点
                const std::string& host = info.host;
                last_candidate_host = host;
                
                // 检查可用性
                bool is_excluded = (excluded.find(host) != excluded.end());
                bool circuit_break = !ha::CircuitBreaker::instance().should_access(normalizeHostKey(host));
                
                if (!is_excluded && !circuit_break) {
                     auto res = info.weight->AddInflight(index, dice, left, begin_time_us, PUNISH_INFLIGHT_RATIO, MIN_WEIGHT);
                     
                     if (res.weight_diff != 0) {
                         UpdateParentWeights(*servers, res.weight_diff, index);
                         servers->total.fetch_add(res.weight_diff);
                         total = servers->total.load(); 
                     }
                     
                     if (res.chosen) {
                         return host;
                     }
                }
                found = true; 
                break; 
            } else {
                dice -= (left + self);
                index = index * 2 + 2;
            }
        }
        
        if (!found) {
            total = servers->total.load();
        }
    }
    
    // 兜底
    if (!servers->weight_tree.empty()) {
        std::uniform_int_distribution<size_t> dist(0, servers->weight_tree.size() - 1);
        return servers->weight_tree[dist(generator)].host;
    }
    
    return "";
}

void LalbManager::Feedback(const std::string& host, bool success, int64_t begin_time_us, int64_t end_time_us, int64_t timeout_ms, int retried_count) {
    butil::DoublyBufferedData<LalbServers>::ScopedPtr servers;
    if (_db_servers.Read(&servers) != 0) {
        return;
    }
    
    auto it = servers->server_map.find(host);
    if (it == servers->server_map.end()) return;
    size_t index = it->second;
    
    LalbWeight* w = servers->weight_tree[index].weight;
    
    int64_t diff = w->Update(success, begin_time_us, end_time_us, timeout_ms, retried_count, 
              PUNISH_ERROR_RATIO, WEIGHT_SCALE, MIN_WEIGHT, PUNISH_INFLIGHT_RATIO, index);
    
    if (diff != 0) {
        UpdateParentWeights(*servers, diff, index);
        servers->total.fetch_add(diff);
    }
}

static std::unordered_map<std::string, std::unique_ptr<LalbManager>> g_lalb_managers;
static std::mutex g_lalb_mutex;

LalbManager& GetLalbManager(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_lalb_mutex);
    if (g_lalb_managers.find(key) == g_lalb_managers.end()) {
        g_lalb_managers[key] = std::make_unique<LalbManager>();
    }
    return *g_lalb_managers[key];
}

std::string LalbLB::select(const SelectIn& in) {
    GetLalbManager(in.service_key).EnsureHosts(in.hosts);
    
    std::set<std::string> excluded_copy;
    if (in.excluded) excluded_copy = *in.excluded;
    
    return GetLalbManager(in.service_key).Select(excluded_copy, in.begin_time_us);
}

void LalbLB::feedback(const CallInfo& info) {
    GetLalbManager(info.service_key).Feedback(info.host, info.success, info.begin_time_us, info.end_time_us, info.timeout_ms, info.retried_count);
}

void LalbFeedback(const std::string& key, const std::string& host, bool success, int64_t begin_us, int64_t end_us, int64_t timeout_ms, int retried_count) {
    GetLalbManager(key).Feedback(host, success, begin_us, end_us, timeout_ms, retried_count);
}
