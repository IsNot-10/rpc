#pragma once
#include <cstdint>
#include <mutex>
#include <vector>
#include <cstddef>
#include <atomic>

/**
 * @brief 延迟感知负载均衡权重管理类
 * 
 * 负责管理单个节点的动态权重，根据节点的实际延迟情况调整权重值
 * 实现了基于延迟的自适应权重调整算法
 */
class LalbWeight {
public:
    /**
     * @brief 构造函数
     * 
     * @param initial_weight 初始权重值
     */
    explicit LalbWeight(int64_t initial_weight);
    
    /**
     * @brief 基于平均延迟初始化权重
     * 
     * @param avg_latency 平均延迟值
     */
    void Seed(double avg_latency);
    
    /**
     * @brief 添加在飞请求结果结构体
     * 
     * 包含选择结果和权重差异信息
     */
    struct AddInflightResult {
        bool chosen;       ///< 节点是否被选中
        int64_t weight_diff; ///< 权重差异值
    };
    
    /**
     * @brief 尝试选择该节点
     * 
     * 检查节点的可用权重，决定是否选择该节点
     * 
     * @param index 节点索引
     * @param dice 随机数
     * @param left 剩余权重
     * @param now_us 当前时间（微秒）
     * @return AddInflightResult 选择结果
     */
    AddInflightResult AddInflight(size_t index, int64_t dice, int64_t left, int64_t now_us);
    
    /**
     * @brief 标记节点失败
     * 
     * 当节点调用失败时，更新权重
     * 
     * @param index 节点索引
     * @param avg_weight 平均权重
     * @return int64_t 更新后的权重值
     */
    int64_t MarkFailed(size_t index, int64_t avg_weight);
    
    /**
     * @brief 基于请求完成反馈更新权重
     * 
     * 根据请求的执行结果，动态调整节点权重
     * 
     * @param success 请求是否成功
     * @param begin_time_us 请求开始时间（微秒）
     * @param end_time_us 请求结束时间（微秒）
     * @param timeout_ms 请求超时时间（毫秒）
     * @param retried_count 重试次数
     * @param index 节点索引
     * @return int64_t 更新后的权重值
     */
    int64_t Update(bool success, int64_t begin_time_us, int64_t end_time_us, int64_t timeout_ms, int retried_count, size_t index);
    
    /**
     * @brief 设置权重值
     * 
     * @param weight 新的权重值
     */
    void SetWeight(int64_t weight);

    /**
     * @brief 禁用节点
     * 
     * @return int64_t 禁用前的权重值
     */
    int64_t Disable();
    
    /**
     * @brief 获取当前权重值
     * 
     * @return int64_t 当前权重值
     */
    int64_t Value() const;
    
    /**
     * @brief 获取平均延迟
     * 
     * @return int64_t 平均延迟值（微秒）
     */
    int64_t AvgLatency() const;
    
    /**
     * @brief 树节点当前权重的共享原子变量
     * 
     * 用于在权重树中共享节点的当前权重值
     */
    std::atomic<int64_t> shared_weight;

private:
    static const int64_t DEFAULT_QPS = 1;           ///< 默认 QPS
    static const size_t RECV_QUEUE_SIZE = 128;       ///< 接收队列大小
    
    /**
     * @brief 时间信息结构体
     * 
     * 记录请求的延迟和结束时间
     */
    struct TimeInfo {
        int64_t latency_sum;         ///< 延迟前缀和（微秒）
        int64_t end_time_us;     ///< 请求结束时间（微秒）
    };
    
    /**
     * @brief 重置权重
     * 
     * @param index 节点索引
     * @param now_us 当前时间（微秒）
     * @return int64_t 重置后的权重值
     */
    int64_t ResetWeight(size_t index, int64_t now_us);
    
    int64_t _weight;           ///< 当前动态权重
    int64_t _base_weight;      ///< 基础权重
    mutable std::mutex _mutex; ///< 保护内部状态的互斥锁
    int64_t _begin_time_sum;   ///< 开始时间总和
    int _begin_time_count;     ///< 开始时间计数
    int64_t _old_diff_sum;     ///< 旧的差异总和
    size_t _old_index;         ///< 旧的索引
    int64_t _avg_latency;      ///< 平均延迟（微秒）
    
    // 时间信息循环缓冲区
    std::vector<TimeInfo> _time_q;     ///< 存储时间信息的循环缓冲区
    size_t _time_q_start = 0;          ///< 循环缓冲区中最旧元素的索引
    size_t _time_q_count = 0;          ///< 循环缓冲区中的元素数量
};
