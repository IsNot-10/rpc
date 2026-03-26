#pragma once
#include <string>
#include <map>
#include <mutex>
#include <atomic>
#include <memory>
#include <shared_mutex>

/**
 * @brief 高可用模块命名空间
 * 
 * 包含高可用相关的组件：
 * - 并发限制器
 * - 熔断机制
 * - 速率限制器
 */
namespace ha {

/**
 * @brief 并发限制器类
 * 
 * 管理每个主机的在飞请求数量，确保系统不会因并发过高而崩溃
 * 使用 shared_mutex 进行 map 访问，atomic 计数器存储值，以提高并发性能
 */
class ConcurrencyLimiter {
public:
    /**
     * @brief 获取单例实例
     * 
     * @return ConcurrencyLimiter& 单例实例的引用
     */
    static ConcurrencyLimiter& instance();
    
    /**
     * @brief 增加主机的在飞请求计数
     * 
     * @param host 主机地址，格式为 "ip:port"
     */
    void inc(const std::string& host);
    
    /**
     * @brief 减少主机的在飞请求计数
     * 
     * @param host 主机地址，格式为 "ip:port"
     */
    void dec(const std::string& host);
    
    /**
     * @brief 获取主机当前的在飞请求计数
     * 
     * @param host 主机地址，格式为 "ip:port"
     * @return int 当前在飞请求数量
     */
    int get(const std::string& host);

    /**
     * @brief 设置全局最大并发限制
     * 
     * @param limit 最大并发数，0 表示无限制
     */
    void set_max_concurrency(int limit);
    
    /**
     * @brief 检查新请求是否被允许
     * 
     * 根据当前在飞请求数量和最大并发限制，判断是否允许新请求
     * 
     * @param host 主机地址，格式为 "ip:port"
     * @return bool 允许请求返回 true，否则返回 false
     */
    bool is_allowed(const std::string& host);

private:
    /**
     * @brief 获取或创建主机的计数器
     * 
     * 线程安全地获取主机的计数器，如果不存在则创建
     * 
     * @param host 主机地址，格式为 "ip:port"
     * @return std::shared_ptr<std::atomic<int>> 主机的计数器智能指针
     */
    std::shared_ptr<std::atomic<int>> get_counter(const std::string& host);

    std::map<std::string, std::shared_ptr<std::atomic<int>>> inflight_map_; ///< 主机到在飞请求计数器的映射
    std::shared_mutex mutex_; ///< 保护 inflight_map_ 的共享互斥锁，支持并发读取
    std::atomic<int> max_concurrency_; ///< 全局最大并发限制，0 表示无限制
};

} // namespace ha
