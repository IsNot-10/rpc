#include "lb/lalb_manager.h"
#include "lb/lb_common.h"
#include "Logging.h"
#include "../ha/circuit_breaker.h"
#include "../ha/concurrency_limiter.h"
#include <cmath>
#include <numeric>
#include <algorithm>
#include <random>

#include <limits>

/**
 * @brief 权重缩放因子
 * 
 * 用于将 QPS 和延迟转换为权重，确保权重值在安全范围内
 */
const int64_t LalbManager::WEIGHT_SCALE = std::numeric_limits<int64_t>::max() / 72000000 / (128 - 1);

/**
 * @brief 最小权重值
 * 
 * 确保每个节点至少有一个最小权重，避免节点被饿死
 */
const int64_t LalbManager::MIN_WEIGHT = 1000; 

/**
 * @brief 在飞请求惩罚因子
 * 
 * 当在飞请求延迟超过平均延迟的该倍数时，会降低节点权重
 * BRPC default is 1.5
 */
const double LalbManager::PUNISH_INFLIGHT_RATIO = 1.5; 

/**
 * @brief 错误惩罚因子
 * 
 * 当请求失败时，会将延迟乘以该因子作为惩罚
 */
const double LalbManager::PUNISH_ERROR_RATIO = 1.2;

/**
 * @brief 延迟感知负载均衡管理器的构造函数
 */
LalbManager::LalbManager() {}

/**
 * @brief 更新权重树中父节点的权重
 * 
 * 当某个节点的权重发生变化时，需要向上更新其父节点的权重
 * 
 * @param servers 服务器列表
 * @param diff 权重变化量
 * @param index 发生变化的节点索引
 */
void LalbManager::UpdateParentWeights(const LalbServers& servers, int64_t diff, size_t index) {
    // 1. 更新当前节点的 current_weight
    if (index < servers.weight_tree.size()) {
        servers.weight_tree[index].current_weight->fetch_add(diff, std::memory_order_relaxed);
    }
    
    // 2. 向上更新父节点的 left sum
    while (index > 0) {
        size_t parent = (index - 1) / 2; ///< 计算父节点索引
        if (parent * 2 + 1 == index) { // 如果是左子节点，父节点的 left 需要增加
            if (parent < servers.left_weights.size()) {
                servers.left_weights[parent].val.fetch_add(diff, std::memory_order_relaxed);
            }
        }
        // 如果是右子节点，父节点的 left 不需要改变，但是我们需要继续向上遍历！
        // 因为 parent 的父节点可能需要更新（如果 parent 是它父节点的左子节点）
        index = parent; ///< 继续向上更新父节点
    }
}

/**
 * @brief 确保所有主机都在权重树中注册
 * 
 * 检查主机列表是否发生变化，如果有变化则更新权重树
 * 
 * @param raw_hosts 原始主机列表，可能包含权重信息（格式：ip:port:weight）
 */
void LalbManager::EnsureHosts(const std::vector<std::string>& raw_hosts) {
    // 归一化主机列表，去除权重信息
    std::vector<std::string> hosts;
    hosts.reserve(raw_hosts.size());
    for (const auto& h : raw_hosts) {
         std::string key = normalizeHostKey(h); ///< 归一化主机 Key
         hosts.push_back(key);
    }
    
    // 优化：检查主机列表是否发生变化，避免频繁修改（减少锁竞争）
    {
        butil::DoublyBufferedData<LalbServers>::ScopedPtr servers;
        if (_db_servers.Read(&servers) == 0) {
             if (servers->server_map.size() == hosts.size()) {
                 bool match = true;
                 for (const auto& h : hosts) {
                     if (servers->server_map.find(h) == servers->server_map.end()) {
                         match = false;
                         break;
                     }
                 }
                 if (match) return; ///< 主机列表未发生变化，直接返回
             }
        }
    }
    
    // 准备权重对象 (需要在 Modify 之外进行，以减少锁竞争)
    std::vector<LalbWeight*> host_weights;
    host_weights.reserve(hosts.size());
    
    {
        std::lock_guard<std::mutex> lock(_weights_mutex); ///< 加锁保护权重映射
        for (const auto& host : hosts) {
            if (_weights.find(host) == _weights.end()) {
                _weights[host] = new LalbWeight(WEIGHT_SCALE); ///< 为新主机创建权重对象
            }
            host_weights.push_back(_weights[host]); ///< 添加到权重列表
        }
    }

    // 定义更新函数，用于修改双缓冲数据结构
    auto update_fn = [&](LalbServers& servers) {
        // 清空现有数据
        servers.server_map.clear();
        servers.weight_tree.clear();
        servers.left_weights.clear();
        
        // 预分配内存
        servers.weight_tree.reserve(hosts.size());
        servers.left_weights.reserve(hosts.size());
        
        // 初始化节点
        for (size_t i = 0; i < hosts.size(); ++i) {
            const auto& host = hosts[i];
            LalbWeight* w = host_weights[i];
            
            servers.left_weights.emplace_back(0); ///< 添加左权重节点
            
            LalbServerInfo info; ///< 创建服务器信息
            info.host = host; ///< 设置主机地址
            info.weight = w; ///< 设置权重对象
            
            // 同步共享权重与逻辑权重
            w->shared_weight.store(w->Value());
            info.current_weight = &w->shared_weight; ///< 设置当前权重指针
            
            servers.weight_tree.push_back(info); ///< 添加到权重树
            servers.server_map[host] = i; ///< 更新服务器映射
        }
        
        // 修复指针 (vector 填充完毕，地址稳定)
        for (size_t i = 0; i < hosts.size(); ++i) {
            servers.weight_tree[i].left = &servers.left_weights[i].val; ///< 设置左权重指针
            // current_weight 已经设置为共享原子变量
        }
        
        // 构建树并计算 subtree weights (自底向上)
        size_t n = servers.weight_tree.size();
        if (n > 0) {
            std::vector<int64_t> subtree_weights(n, 0); ///< 存储子树权重
            for (int i = n - 1; i >= 0; --i) {
                int64_t w = servers.weight_tree[i].current_weight->load(); ///< 获取当前节点权重
                size_t l_idx = 2 * i + 1; ///< 左子节点索引
                size_t r_idx = 2 * i + 2; ///< 右子节点索引
                
                int64_t l_sum = (l_idx < n) ? subtree_weights[l_idx] : 0; ///< 左子树权重和
                int64_t r_sum = (r_idx < n) ? subtree_weights[r_idx] : 0; ///< 右子树权重和
                
                servers.left_weights[i].val.store(l_sum); ///< 存储左子树权重和
                subtree_weights[i] = w + l_sum + r_sum; ///< 计算当前节点的子树权重和
            }
            servers.total.store(subtree_weights[0]); ///< 设置总权重
        } else {
            servers.total.store(0); ///< 空树，总权重为 0
        }
        return 1; ///< 返回 1 表示修改成功
    };
    
    _db_servers.Modify(update_fn); ///< 调用 Modify 更新双缓冲数据结构
    
    // 简单的日志，无法获取 total_weight 除非从 servers 读取
    LOG_INFO << "LALB updated hosts, count=" << hosts.size();
}

/**
 * @brief 根据延迟感知算法选择一个最佳节点
 * 
 * 实现了延迟感知负载均衡算法，根据节点的延迟情况动态调整权重
 * 
 * @param excluded 排除的节点集合
 * @param begin_time_us 请求开始时间（微秒）
 * @return std::string 选中的节点，格式为 "ip:port"
 */
std::string LalbManager::Select(const std::set<std::string>& excluded, int64_t begin_time_us) {
    butil::DoublyBufferedData<LalbServers>::ScopedPtr servers;
    if (_db_servers.Read(&servers) != 0) {
        return ""; ///< 读取失败，返回空字符串
    }
    
    if (servers->weight_tree.empty()) return ""; ///< 权重树为空，返回空字符串
    
    int64_t total = servers->total.load(); ///< 获取总权重
    if (total <= 0) return ""; ///< 总权重为 0，返回空字符串
    
    size_t nloop = 0; ///< 循环次数计数器
    std::string last_candidate_host; ///< 记录上次候选主机
    
    while (total > 0) {
        if (++nloop > 1000) { ///< 避免无限循环
             // Fallback: Linear scan to find any valid non-excluded host
             // 当高权重节点被排除（如重试时）可能导致死循环，此时应降级为轮询查找可用节点
             size_t start_idx = lb::fast_rand_less_than(servers->weight_tree.size());
             for (size_t i = 0; i < servers->weight_tree.size(); ++i) {
                 size_t idx = (start_idx + i) % servers->weight_tree.size();
                 const auto& host = servers->weight_tree[idx].host;
                 bool is_excluded = (excluded.find(host) != excluded.end());
                 bool circuit_break = !ha::CircuitBreaker::instance().should_access(normalizeHostKey(host));
                 
                 if (!is_excluded && !circuit_break) {
                     return host;
                 }
             }

             if (!last_candidate_host.empty()) return last_candidate_host; ///< 返回上次候选主机
             break;
        }
        
        // 快速随机选择
        int64_t dice = lb::fast_rand_less_than(total); ///< 生成随机数
        
        size_t index = 0; ///< 从根节点开始
        size_t n = servers->weight_tree.size(); ///< 节点数量
        
        bool found = false; ///< 是否找到候选节点
        while (index < n) {
            const auto& info = servers->weight_tree[index]; ///< 当前节点信息
            // 使用 relaxed ordering 提高性能
            int64_t left = info.left->load(std::memory_order_relaxed); ///< 左子树权重和
            int64_t self = info.current_weight->load(std::memory_order_relaxed); ///< 当前节点权重
            
            if (dice < left) { ///< 随机数落在左子树，递归查找左子树
                index = index * 2 + 1; ///< 左子节点索引
            } else if (dice < left + self) { ///< 随机数落在当前节点
                // 选中当前节点
                const std::string& host = info.host; ///< 获取主机地址
                last_candidate_host = host; ///< 记录候选主机
                
                // 检查可用性
                bool is_excluded = (excluded.find(host) != excluded.end()); ///< 检查是否在排除列表中
                bool circuit_break = !ha::CircuitBreaker::instance().should_access(normalizeHostKey(host)); ///< 检查熔断器状态
                
                if (!is_excluded && !circuit_break) { ///< 节点可用
                     auto res = info.weight->AddInflight(index, dice, left, begin_time_us); ///< 尝试添加在飞请求
                     
                     if (res.weight_diff != 0) { ///< 权重发生变化
                         UpdateParentWeights(*servers, res.weight_diff, index); ///< 更新父节点权重
                         servers->total.fetch_add(res.weight_diff); ///< 更新总权重
                         total = servers->total.load(); ///< 重新获取总权重
                     }
                     
                     if (res.chosen) { ///< 成功选中节点
                         return host; ///< 返回选中的节点
                     }
                }
                found = true; ///< 找到候选节点
                break; 
            } else { ///< 随机数落在右子树，递归查找右子树
                dice -= (left + self); ///< 更新随机数
                index = index * 2 + 2; ///< 右子节点索引
            }
        }
        
        if (!found) {
            total = servers->total.load(std::memory_order_relaxed); ///< 重新获取总权重
        }
    }
    
    // 兜底策略：如果无法通过权重选择，随机选择一个节点
    if (!servers->weight_tree.empty()) {
        // 快速随机选择
        size_t idx = lb::fast_rand_less_than(servers->weight_tree.size()); ///< 随机索引
        return servers->weight_tree[idx].host; ///< 返回随机选中的节点
    }
    
    return ""; ///< 没有可用节点，返回空字符串
}

/**
 * @brief 根据请求结果更新节点权重
 * 
 * 根据请求的成功/失败状态和延迟情况，更新节点的权重
 * 
 * @param host 主机地址，格式为 "ip:port"
 * @param success 请求是否成功
 * @param begin_time_us 请求开始时间（微秒）
 * @param end_time_us 请求结束时间（微秒）
 * @param timeout_ms 请求超时时间（毫秒）
 * @param retried_count 重试次数
 */
void LalbManager::Feedback(const std::string& host, bool success, int64_t begin_time_us, int64_t end_time_us, int64_t timeout_ms, int retried_count) {
    butil::DoublyBufferedData<LalbServers>::ScopedPtr servers;
    if (_db_servers.Read(&servers) != 0) {
        return; ///< 读取失败，返回
    }
    
    auto it = servers->server_map.find(host); ///< 查找主机在映射中的位置
    if (it == servers->server_map.end()) return; ///< 主机不在映射中，返回
    size_t index = it->second; ///< 获取主机索引
    
    LalbWeight* w = servers->weight_tree[index].weight; ///< 获取权重对象
    
    // 更新权重
    int64_t diff = w->Update(success, begin_time_us, end_time_us, timeout_ms, retried_count, index);
    
    if (diff != 0) { ///< 权重发生变化
        UpdateParentWeights(*servers, diff, index); ///< 更新父节点权重
        servers->total.fetch_add(diff); ///< 更新总权重
    }
}

// --- 全局管理器访问 --- //

/**
 * @brief 全局延迟感知负载均衡管理器映射
 * 
 * 每个服务对应一个 LalbManager 实例
 */
static std::unordered_map<std::string, std::unique_ptr<LalbManager>> g_lalb_managers;

/**
 * @brief 保护全局管理器映射的互斥锁
 */
static std::mutex g_lalb_mutex;

/**
 * @brief 获取延迟感知负载均衡管理器
 * 
 * 单例模式，根据服务键获取对应的管理器实例
 * 
 * @param key 服务键，格式为 ServiceName:MethodName
 * @return LalbManager& 管理器实例的引用
 */
LalbManager& GetLalbManager(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_lalb_mutex); ///< 加锁保护临界区
    if (g_lalb_managers.find(key) == g_lalb_managers.end()) {
        // 管理器不存在，创建新实例
        g_lalb_managers[key] = std::make_unique<LalbManager>();
    }
    return *g_lalb_managers[key]; ///< 返回管理器实例的引用
}

// --- LalbLB 实现 --- //

/**
 * @brief 延迟感知负载均衡器的选择方法
 * 
 * 实现了 LoadBalancer 接口的 select 方法，使用延迟感知算法选择节点
 * 
 * @param in 选择输入参数
 * @return std::string 选中的节点，格式为 "ip:port"
 */
std::string LalbLB::select(const SelectIn& in) {
    GetLalbManager(in.service_key).EnsureHosts(in.hosts); ///< 确保所有主机都已注册
    
    std::set<std::string> excluded_copy;
    if (in.excluded) excluded_copy = *in.excluded; ///< 复制排除列表
    
    // 调用管理器的 Select 方法选择节点
    return GetLalbManager(in.service_key).Select(excluded_copy, in.begin_time_us);
}

/**
 * @brief 延迟感知负载均衡器的反馈方法
 * 
 * 实现了 LoadBalancer 接口的 feedback 方法，用于反馈请求结果
 * 
 * @param info 调用信息，包含请求的结果和延迟等信息
 */
void LalbLB::feedback(const CallInfo& info) {
    GetLalbManager(info.service_key).Feedback(info.host, info.success, info.begin_time_us, info.end_time_us, info.timeout_ms, info.retried_count);
}

/**
 * @brief 延迟感知负载均衡器的全局反馈函数
 * 
 * 用于向指定服务的延迟感知负载均衡管理器反馈请求结果
 * 
 * @param key 服务键，格式为 ServiceName:MethodName
 * @param host 主机地址，格式为 "ip:port"
 * @param success 请求是否成功
 * @param begin_us 请求开始时间（微秒）
 * @param end_us 请求结束时间（微秒）
 * @param timeout_ms 请求超时时间（毫秒）
 * @param retried_count 重试次数
 */
void LalbFeedback(const std::string& key, const std::string& host, bool success, int64_t begin_us, int64_t end_us, int64_t timeout_ms, int retried_count) {
    GetLalbManager(key).Feedback(host, success, begin_us, end_us, timeout_ms, retried_count);
}
