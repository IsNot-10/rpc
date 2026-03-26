#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <mutex>
#include <thread>
#include <map>
#include <cstdarg>
#include <deque>
#include "json.h" // nlohmann::json

namespace mprpc {
/**
 * @brief 分布式追踪命名空间
 * 
 * 实现了基于OpenTracing规范的分布式追踪系统
 * 支持跨服务、跨进程的请求追踪
 */
namespace tracing {

/**
 * @brief Span类型枚举
 * 
 * 表示Span在分布式系统中的角色
 */
enum class SpanKind {
    CLIENT,   ///< 客户端Span，发起请求的一方
    SERVER,   ///< 服务器Span，处理请求的一方
    INTERNAL  ///< 内部Span，同一服务内的操作
};

/**
 * @brief 注解结构体
 * 
 * 用于记录Span执行过程中的关键事件
 */
struct Annotation {
    int64_t timestamp_us; ///< 事件发生的时间戳（微秒）
    std::string value;    ///< 注解内容
};

/**
 * @brief Span类
 * 
 * 表示分布式追踪中的一个操作单元
 * 包含操作的开始时间、结束时间、标签和注解等信息
 */
class Span {
public:
    /**
     * @brief 构造函数
     * 
     * @param trace_id 跟踪ID，全局唯一
     * @param span_id Span ID，在跟踪内唯一
     * @param parent_span_id 父Span ID，0表示根Span
     * @param name Span名称，通常是服务方法名
     * @param kind Span类型
     */
    Span(uint64_t trace_id, uint64_t span_id, uint64_t parent_span_id, 
         const std::string& name, SpanKind kind);
    ~Span() = default;

    // Factory methods matching BRPC style
    /**
     * @brief 创建客户端Span
     * 
     * @param name Span名称
     * @return std::shared_ptr<Span> 创建的Span指针
     */
    static std::shared_ptr<Span> CreateClientSpan(const std::string& name);
    
    /**
     * @brief 创建服务器Span
     * 
     * @param trace_id_str 跟踪ID字符串（十六进制）
     * @param span_id_str Span ID字符串（十六进制）
     * @param parent_span_id_str 父Span ID字符串（十六进制）
     * @param name Span名称
     * @return std::shared_ptr<Span> 创建的Span指针
     */
    static std::shared_ptr<Span> CreateServerSpan(const std::string& trace_id_str, const std::string& span_id_str, 
                                                const std::string& parent_span_id_str, const std::string& name);
    
    /**
     * @brief 创建服务器Span
     * 
     * @param trace_id 跟踪ID
     * @param span_id Span ID
     * @param parent_span_id 父Span ID
     * @param name Span名称
     * @return std::shared_ptr<Span> 创建的Span指针
     */
    static std::shared_ptr<Span> CreateServerSpan(uint64_t trace_id, uint64_t span_id, uint64_t parent_span_id, 
                                                const std::string& name);

    /**
     * @brief 添加注解
     * 
     * @param value 注解内容
     */
    void AddAnnotation(const std::string& value);
    
    /**
     * @brief 格式化添加注解
     * 
     * @param fmt 格式化字符串
     * @param ... 可变参数
     */
    void Annotate(const char* fmt, ...);
    
    // Setters for BRPC-like metrics
    
    /**
     * @brief 设置远程地址
     * 
     * @param addr 远程地址，格式为 "ip:port"
     */
    void SetRemoteSide(const std::string& addr) { remote_side_ = addr; }
    
    /**
     * @brief 设置请求大小
     * 
     * @param size 请求大小（字节）
     */
    void SetRequestSize(int size) { request_size_ = size; }
    
    /**
     * @brief 设置响应大小
     * 
     * @param size 响应大小（字节）
     */
    void SetResponseSize(int size) { response_size_ = size; }
    
    /**
     * @brief 设置错误码
     * 
     * @param code 错误码，0表示成功
     */
    void SetErrorCode(int code) { error_code_ = code; }

    // Fine-grained timestamps (BRPC style)
    
    /**
     * @brief 设置实际接收时间
     * 
     * @param t 时间戳（微秒）
     */
    void SetReceivedRealUs(int64_t t) { received_real_us_ = t; }
    
    /**
     * @brief 设置开始解析时间
     * 
     * @param t 时间戳（微秒）
     */
    void SetStartParseRealUs(int64_t t) { start_parse_real_us_ = t; }
    
    /**
     * @brief 设置开始回调时间
     * 
     * @param t 时间戳（微秒）
     */
    void SetStartCallbackRealUs(int64_t t) { start_callback_real_us_ = t; }
    
    /**
     * @brief 设置开始发送时间
     * 
     * @param t 时间戳（微秒）
     */
    void SetStartSendRealUs(int64_t t) { start_send_real_us_ = t; }
    
    /**
     * @brief 设置发送完成时间
     * 
     * @param t 时间戳（微秒）
     */
    void SetSentRealUs(int64_t t) { sent_real_us_ = t; }
    
    /**
     * @brief 添加标签
     * 
     * @param key 标签键
     * @param value 标签值
     */
    void AddTag(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        tags_[key] = value;
    }

    /**
     * @brief 结束Span
     * 
     * 设置结束时间，计算持续时间，并上报Span
     */
    void End();

    // Getters
    
    /**
     * @brief 获取跟踪ID
     * 
     * @return uint64_t 跟踪ID
     */
    uint64_t trace_id() const { return trace_id_; }
    
    /**
     * @brief 获取Span ID
     * 
     * @return uint64_t Span ID
     */
    uint64_t span_id() const { return span_id_; }
    
    /**
     * @brief 获取父Span ID
     * 
     * @return uint64_t 父Span ID
     */
    uint64_t parent_span_id() const { return parent_span_id_; }
    
    std::string Name() const { return name_; }

    /**
     * @brief 获取跟踪ID的十六进制字符串
     * 
     * @return std::string 跟踪ID的十六进制字符串
     */
    std::string TraceIdStr() const;
    
    /**
     * @brief 获取Span ID的十六进制字符串
     * 
     * @return std::string Span ID的十六进制字符串
     */
    std::string SpanIdStr() const;
    
    /**
     * @brief 获取父Span ID的十六进制字符串
     * 
     * @return std::string 父Span ID的十六进制字符串
     */
    std::string ParentSpanIdStr() const;

    /**
     * @brief 将Span转换为JSON对象
     * 
     * @return nlohmann::json JSON对象
     */
    nlohmann::json ToJson() const;

private:
    /**
     * @brief 上报Span
     * 
     * 将Span信息输出到日志
     */
    void Report(); // Output to log

    uint64_t trace_id_; ///< 跟踪ID，全局唯一
    uint64_t span_id_; ///< Span ID，在跟踪内唯一
    uint64_t parent_span_id_; ///< 父Span ID，0表示根Span
    
    std::string name_; ///< Span名称，通常是服务方法名
    SpanKind kind_; ///< Span类型
    
    int64_t start_time_us_; ///< 开始时间戳（微秒）
    int64_t end_time_us_ = 0; ///< 结束时间戳（微秒）
    int64_t duration_us_ = 0; ///< 持续时间（微秒）
    
    // BRPC style timestamps
    int64_t received_real_us_ = 0; ///< 实际接收时间
    int64_t start_parse_real_us_ = 0; ///< 开始解析时间
    int64_t start_callback_real_us_ = 0; ///< 开始回调时间
    int64_t start_send_real_us_ = 0; ///< 开始发送时间
    int64_t sent_real_us_ = 0; ///< 发送完成时间

    std::string remote_side_; ///< 远程地址
    int request_size_ = 0; ///< 请求大小（字节）
    int response_size_ = 0; ///< 响应大小（字节）
    int error_code_ = 0; ///< 错误码，0表示成功

    std::vector<Annotation> annotations_; ///< 注解列表
    std::map<std::string, std::string> tags_; ///< 标签映射
    mutable std::mutex mutex_; ///< 保护注解和标签的互斥锁
};

/**
 * @brief 跟踪上下文类
 * 
 * 用于管理当前线程的跟踪上下文
 * 提供创建Span、获取当前Span等功能
 */
class TraceContext {
public:
    /**
     * @brief 生成唯一ID
     * 
     * @return uint64_t 生成的唯一ID
     */
    static uint64_t GenerateId();

    /**
     * @brief 清除当前线程的跟踪信息
     */
    static void Clear();

    /**
     * @brief 上下文快照结构体
     * 
     * 用于在线程间传递跟踪上下文
     */
    struct ContextSnapshot {
        std::shared_ptr<Span> span; ///< 当前Span
    };

    /**
     * @brief 获取当前上下文快照
     * 
     * @return ContextSnapshot 当前上下文快照
     */
    static ContextSnapshot GetSnapshot();
    
    /**
     * @brief 恢复上下文快照
     * 
     * @param snapshot 上下文快照
     */
    static void RestoreSnapshot(const ContextSnapshot& snapshot);

    /**
     * @brief 获取当前Span
     * 
     * @return std::shared_ptr<Span> 当前Span
     */
    static std::shared_ptr<Span> GetCurrentSpan();
    
    /**
     * @brief 设置当前Span
     * 
     * @param span 当前Span
     */
    static void SetCurrentSpan(std::shared_ptr<Span> span);

    /**
     * @brief 格式化跟踪打印
     * 
     * @tparam Args 可变参数类型
     * @param fmt 格式化字符串
     * @param args 可变参数
     */
    template<typename... Args>
    static void TracePrintf(const char* fmt, Args... args) {
        auto span = GetCurrentSpan();
        if (span) {
            span->Annotate(fmt, args...);
        }
    }

private:
    static thread_local std::shared_ptr<Span> current_span_; ///< 当前线程的Span
};

} // namespace tracing
} // namespace mprpc

/**
 * @brief 跟踪打印宏
 * 
 * 在当前Span中添加格式化注解
 * 
 * @param fmt 格式化字符串
 * @param ... 可变参数
 */
#define TRACEPRINTF(fmt, ...) \
    mprpc::tracing::TraceContext::TracePrintf(fmt, ##__VA_ARGS__)
