#pragma once
#include <string>
#include <map>
#include <memory>
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace ha {

/**
 * @brief 基于指数移动平均（EMA）的错误记录器
 * 
 * 记录错误率和延迟，使用指数移动平均算法
 * 适配自 BRPC 框架的相关实现
 */
class EmaErrorRecorder {
public:
    /**
     * @brief 构造函数
     * 
     * @param window_size 滑动窗口大小
     * @param max_error_percent 最大错误百分比，超过此值则认为不健康
     */
    EmaErrorRecorder(int window_size, int max_error_percent);
    
    /**
     * @brief 调用结束时更新记录
     * 
     * @param error_code 错误码，0表示成功，非0表示失败
     * @param latency 调用延迟（微秒）
     * @return bool 节点健康返回true，不健康返回false
     */
    bool on_call_end(int error_code, int64_t latency);
    
    /**
     * @brief 重置记录器状态
     */
    void reset();
    
    /**
     * @brief 获取当前错误率
     * 
     * @return double 错误率，范围 [0, 1]
     */
    double get_error_rate() const;

private:
    /**
     * @brief 更新延迟统计
     * 
     * @param latency 调用延迟（微秒）
     * @return int64_t 更新后的EMA延迟
     */
    int64_t update_latency(int64_t latency);
    
    /**
     * @brief 更新错误成本
     * 
     * @param latency 调用延迟（微秒）
     * @param ema_latency EMA延迟（微秒）
     * @return bool 节点健康返回true，不健康返回false
     */
    bool update_error_cost(int64_t latency, int64_t ema_latency);

    const int window_size_;                     ///< 滑动窗口大小
    const int max_error_percent_;               ///< 最大错误百分比
    const double smooth_;                       ///< 平滑因子
    
    std::atomic<int32_t> sample_count_when_initializing_; ///< 初始化时的样本计数
    std::atomic<int32_t> error_count_when_initializing_;  ///< 初始化时的错误计数
    std::atomic<int64_t> ema_error_cost_;       ///< EMA错误成本
    std::atomic<int64_t> ema_latency_;          ///< EMA延迟
};

/**
 * @brief 熔断节点类
 * 
 * 表示单个服务节点的熔断状态，包含长窗口和短窗口两个EMA记录器
 */
class CircuitBreakerNode {
public:
    /**
     * @brief 构造函数
     */
    CircuitBreakerNode();
    
    /**
     * @brief 析构函数
     */
    ~CircuitBreakerNode() = default;

    /**
     * @brief 调用结束时更新熔断状态
     * 
     * @param error_code 错误码，0表示成功，非0表示失败
     * @param latency 调用延迟（微秒）
     * @return bool 节点健康返回true，需要隔离返回false
     */
    bool on_call_end(int error_code, int64_t latency);
    
    /**
     * @brief 重置内部状态
     */
    void reset();
    
    /**
     * @brief 显式标记节点为故障
     */
    void mark_as_broken();
    
    /**
     * @brief 检查节点是否可用
     * 
     * @return bool 节点可用返回true，否则返回false
     */
    bool is_available();
    
    /**
     * @brief 获取隔离持续时间
     * 
     * @return int 隔离持续时间（毫秒）
     */
    int isolation_duration_ms() const {
        return isolation_duration_ms_.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief 获取隔离次数
     * 
     * @return int 节点被隔离的次数
     */
    int isolated_times() const {
        return isolated_times_.load(std::memory_order_relaxed);
    }

private:
    /**
     * @brief 更新隔离持续时间
     * 
     * 基于隔离次数动态调整隔离时间，采用指数退避策略
     */
    void update_isolation_duration();

    EmaErrorRecorder long_window_;      ///< 长窗口错误记录器
    EmaErrorRecorder short_window_;     ///< 短窗口错误记录器
    
    int64_t last_reset_time_ms_;        ///< 上次重置时间（毫秒）
    std::atomic<int> isolation_duration_ms_; ///< 当前隔离持续时间（毫秒）
    std::atomic<int> isolated_times_;   ///< 隔离次数
    std::atomic<bool> broken_;          ///< 节点是否处于故障状态
    
    std::chrono::steady_clock::time_point broken_until_; ///< 节点恢复可用的时间点
    std::mutex mutex_;                  ///< 保护broken_until_更新的互斥锁
};

/**
 * @brief 熔断管理器类
 * 
 * 管理多个熔断节点，实现单例模式
 */
class CircuitBreaker {
public:
    /**
     * @brief 获取单例实例
     * 
     * @return CircuitBreaker& 单例实例的引用
     */
    static CircuitBreaker& instance();
    
    /**
     * @brief 检查主机是否可访问
     * 
     * @param host 主机地址，格式为 "ip:port"
     * @return bool 主机可访问返回true，否则返回false
     */
    bool should_access(const std::string& host);
    
    /**
     * @brief 报告请求状态
     * 
     * @param host 主机地址，格式为 "ip:port"
     * @param error_code 错误码，0表示成功，非0表示失败
     * @param latency_ms 调用延迟（毫秒）
     */
    void report_status(const std::string& host, int error_code, int64_t latency_ms);
    
    /**
     * @brief 报告请求状态（布尔版本）
     * 
     * @param host 主机地址，格式为 "ip:port"
     * @param success 请求是否成功
     * @param latency_ms 调用延迟（毫秒）
     */
    void report_status(const std::string& host, bool success, int64_t latency_ms);
    
    /**
     * @brief 获取估计延迟
     * 
     * @param host 主机地址，格式为 "ip:port"
     * @return double 估计延迟（毫秒），目前返回-1
     */
    double get_latency(const std::string& host);

    /**
     * @brief 重置指定主机的熔断状态
     * 
     * @param host 主机地址，格式为 "ip:port"
     */
    void reset(const std::string& host);

private:
    /**
     * @brief 获取或创建熔断节点
     * 
     * @param host 主机地址，格式为 "ip:port"
     * @return std::shared_ptr<CircuitBreakerNode> 熔断节点智能指针
     */
    std::shared_ptr<CircuitBreakerNode> get_node(const std::string& host);

    std::map<std::string, std::shared_ptr<CircuitBreakerNode>> nodes_; ///< 主机到熔断节点的映射
    std::shared_mutex mutex_; ///< 保护nodes_的共享互斥锁
};

} // namespace ha
