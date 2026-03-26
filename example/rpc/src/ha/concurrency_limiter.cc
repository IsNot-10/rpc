#include "ha/concurrency_limiter.h"

namespace ha {

/**
 * @brief 获取并发限制器的单例实例
 * 
 * @return ConcurrencyLimiter& 并发限制器实例的引用
 */
ConcurrencyLimiter& ConcurrencyLimiter::instance() {
    static ConcurrencyLimiter inst; ///< 静态局部变量，实现线程安全的单例模式
    return inst;
}

/**
 * @brief 获取指定主机的并发计数器
 * 
 * @param host 主机地址
 * @return std::shared_ptr<std::atomic<int>> 并发计数器的智能指针
 */
std::shared_ptr<std::atomic<int>> ConcurrencyLimiter::get_counter(const std::string& host) {
    // 快速路径：读锁
    {
        std::shared_lock<std::shared_mutex> lock(mutex_); ///< 共享锁
        auto it = inflight_map_.find(host); ///< 查找主机对应的计数器
        if (it != inflight_map_.end()) {
            return it->second; ///< 找到计数器，返回
        }
    }
    
    // 慢速路径：写锁
    std::unique_lock<std::shared_mutex> lock(mutex_); ///< 独占锁
    // 双重检查，防止在获取写锁期间其他线程已创建计数器
    auto it = inflight_map_.find(host);
    if (it != inflight_map_.end()) {
        return it->second; ///< 找到计数器，返回
    }
    
    auto counter = std::make_shared<std::atomic<int>>(0); ///< 创建新的原子计数器
    inflight_map_[host] = counter; ///< 添加到计数器映射
    return counter; ///< 返回新计数器
}

/**
 * @brief 增加指定主机的并发计数
 * 
 * @param host 主机地址
 */
void ConcurrencyLimiter::inc(const std::string& host) {
    auto counter = get_counter(host); ///< 获取计数器
    counter->fetch_add(1, std::memory_order_relaxed); ///< 原子增加计数
}

/**
 * @brief 减少指定主机的并发计数
 * 
 * 确保计数不会变为负数
 * 
 * @param host 主机地址
 */
void ConcurrencyLimiter::dec(const std::string& host) {
    auto counter = get_counter(host); ///< 获取计数器
    // 防止计数变为负数
    int old_val = counter->load(std::memory_order_relaxed); ///< 获取当前计数
    while (old_val > 0) { ///< 只有当计数大于0时才减少
        if (counter->compare_exchange_weak(old_val, old_val - 1, std::memory_order_relaxed)) {
            break; ///< 减少成功，退出循环
        }
    }
}

/**
 * @brief 获取指定主机的当前并发计数
 * 
 * @param host 主机地址
 * @return int 当前并发计数
 */
int ConcurrencyLimiter::get(const std::string& host) {
    auto counter = get_counter(host); ///< 获取计数器
    return counter->load(std::memory_order_relaxed); ///< 返回当前计数
}

/**
 * @brief 设置最大并发限制
 * 
 * @param limit 最大并发数，0表示无限制
 */
void ConcurrencyLimiter::set_max_concurrency(int limit) {
    max_concurrency_.store(limit, std::memory_order_relaxed); ///< 设置最大并发数
}

/**
 * @brief 检查是否允许新的请求
 * 
 * @param host 主机地址
 * @return bool 是否允许新的请求
 */
bool ConcurrencyLimiter::is_allowed(const std::string& host) {
    int max = max_concurrency_.load(std::memory_order_relaxed); ///< 获取最大并发限制
    if (max <= 0) return true; ///< 0表示无限制，允许所有请求
    
    auto counter = get_counter(host); ///< 获取当前并发计数
    return counter->load(std::memory_order_relaxed) < max; ///< 检查是否超过限制
}

} // namespace ha
