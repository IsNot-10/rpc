#include "TraceContext.h"
#include <random>
#include <chrono>
#include <iostream>
#include <cstdarg>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include "Logging.h"

namespace mprpc {
namespace tracing {

// 线程本地存储：当前Span
static thread_local std::shared_ptr<Span> t_current_span;

/**
 * @brief ID生成器结构体
 * 
 * 用于生成唯一的跟踪ID和Span ID
 * 使用线程本地状态避免锁竞争
 */
struct IdGen {
    bool init; ///< 是否已初始化
    uint16_t seq; ///< 序列号，每生成一个ID自增
    uint64_t current_random; ///< 当前随机数
    std::mt19937_64 generator; ///< 随机数生成器
    std::uniform_int_distribution<uint64_t> distribution; ///< 均匀分布
};

// 线程本地ID生成器
static thread_local IdGen t_id_gen = { false, 0, 0 };

/**
 * @brief 初始化ID生成器
 * 
 * 延迟初始化，只在第一次生成ID时执行
 */
static void InitIdGen() {
    if (!t_id_gen.init) {
        t_id_gen.init = true;
        // 使用线程ID和当前时间作为种子，确保唯一性
        uint64_t seed = std::hash<std::thread::id>()(std::this_thread::get_id()) + 
                        std::chrono::system_clock::now().time_since_epoch().count();
        t_id_gen.generator.seed(seed); ///< 初始化随机数生成器
        t_id_gen.current_random = t_id_gen.distribution(t_id_gen.generator); ///< 生成第一个随机数
    }
}

/**
 * @brief 生成唯一ID
 * 
 * @return uint64_t 生成的唯一ID
 */
uint64_t TraceContext::GenerateId() {
    InitIdGen(); ///< 确保ID生成器已初始化
    if (t_id_gen.seq == 0) {
        t_id_gen.current_random = t_id_gen.distribution(t_id_gen.generator); ///< 生成新的随机数
        t_id_gen.seq = 1; ///< 重置序列号
    }
    // 将随机数的高48位与序列号的低16位结合，避免同一线程内的冲突
    return (t_id_gen.current_random & 0xFFFFFFFFFFFF0000ULL) | (t_id_gen.seq++);
}

/**
 * @brief 清除当前线程的跟踪信息
 */
void TraceContext::Clear() {
    t_current_span.reset(); ///< 重置当前Span
}

/**
 * @brief 获取当前上下文快照
 * 
 * @return ContextSnapshot 当前上下文快照
 */
TraceContext::ContextSnapshot TraceContext::GetSnapshot() {
    return {t_current_span}; ///< 返回包含当前Span的快照
}

/**
 * @brief 恢复上下文快照
 * 
 * @param snapshot 上下文快照
 */
void TraceContext::RestoreSnapshot(const ContextSnapshot& snapshot) {
    t_current_span = snapshot.span; ///< 恢复当前Span
}

/**
 * @brief 获取当前Span
 * 
 * @return std::shared_ptr<Span> 当前Span
 */
std::shared_ptr<Span> TraceContext::GetCurrentSpan() {
    return t_current_span; ///< 返回当前Span
}

/**
 * @brief 设置当前Span
 * 
 * @param span 当前Span
 */
void TraceContext::SetCurrentSpan(std::shared_ptr<Span> span) {
    t_current_span = span; ///< 设置当前Span
}

// ------------------------------------------------------------------
// Span Implementation
// ------------------------------------------------------------------

/**
 * @brief Span的构造函数
 * 
 * @param trace_id 跟踪ID，全局唯一
 * @param span_id Span ID，在跟踪内唯一
 * @param parent_span_id 父Span ID，0表示根Span
 * @param name Span名称，通常是服务方法名
 * @param kind Span类型
 */
Span::Span(uint64_t trace_id, uint64_t span_id, uint64_t parent_span_id, 
           const std::string& name, SpanKind kind)
    : trace_id_(trace_id), span_id_(span_id), parent_span_id_(parent_span_id),
      name_(name), kind_(kind) {
    // 设置开始时间戳
    start_time_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * @brief 创建客户端Span
 * 
 * @param name Span名称
 * @return std::shared_ptr<Span> 创建的Span指针
 */
std::shared_ptr<Span> Span::CreateClientSpan(const std::string& name) {
    uint64_t trace_id;
    uint64_t parent_span_id = 0;
    
    auto parent = TraceContext::GetCurrentSpan(); ///< 获取当前Span作为父Span
    if (parent) {
        trace_id = parent->trace_id(); ///< 继承父Span的跟踪ID
        parent_span_id = parent->span_id(); ///< 父Span ID为当前Span的ID
    } else {
        trace_id = TraceContext::GenerateId(); ///< 生成新的跟踪ID
    }
    
    uint64_t span_id = TraceContext::GenerateId(); ///< 生成新的Span ID
    return std::make_shared<Span>(trace_id, span_id, parent_span_id, name, SpanKind::CLIENT);
}

/**
 * @brief 将字符串转换为uint64_t ID
 * 
 * @param str 十六进制字符串
 * @return uint64_t 转换后的ID，失败返回0
 */
static uint64_t ParseId(const std::string& str) {
    if (str.empty()) return 0;
    return std::strtoull(str.c_str(), nullptr, 16);
}

/**
 * @brief 创建服务器Span
 * 
 * @param trace_id_str 跟踪ID字符串（十六进制）
 * @param span_id_str Span ID字符串（十六进制）
 * @param parent_span_id_str 父Span ID字符串（十六进制）
 * @param name Span名称
 * @return std::shared_ptr<Span> 创建的Span指针
 */
std::shared_ptr<Span> Span::CreateServerSpan(const std::string& trace_id_str, const std::string& span_id_str, 
                                            const std::string& parent_span_id_str, const std::string& name) {
    uint64_t trace_id = ParseId(trace_id_str); ///< 解析跟踪ID
    uint64_t parent_span_id = ParseId(parent_span_id_str); ///< 解析父Span ID
    
    // 如果没有跟踪ID（根请求），生成一个新的
    if (trace_id == 0) {
        trace_id = TraceContext::GenerateId();
    }

    // 为自己生成一个新的Span ID
    uint64_t my_span_id = TraceContext::GenerateId();
    
    return std::make_shared<Span>(trace_id, my_span_id, parent_span_id, name, SpanKind::SERVER);
}

/**
 * @brief 创建服务器Span
 * 
 * @param trace_id 跟踪ID
 * @param span_id Span ID
 * @param parent_span_id 父Span ID
 * @param name Span名称
 * @return std::shared_ptr<Span> 创建的Span指针
 */
std::shared_ptr<Span> Span::CreateServerSpan(uint64_t trace_id, uint64_t span_id, uint64_t parent_span_id, 
                                            const std::string& name) {
    return std::make_shared<Span>(trace_id, span_id, parent_span_id, name, SpanKind::SERVER);
}

/**
 * @brief 添加注解
 * 
 * @param value 注解内容
 */
void Span::AddAnnotation(const std::string& value) {
    // 获取当前时间戳
    int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(mutex_); ///< 加锁保护注解列表
    annotations_.push_back({now, value}); ///< 添加注解
}

/**
 * @brief 格式化添加注解
 * 
 * @param fmt 格式化字符串
 * @param ... 可变参数
 */
void Span::Annotate(const char* fmt, ...) {
    char buf[1024]; ///< 格式化缓冲区
    va_list args;
    va_start(args, fmt); ///< 初始化可变参数列表
    vsnprintf(buf, sizeof(buf), fmt, args); ///< 格式化输出到缓冲区
    va_end(args); ///< 结束可变参数列表
    AddAnnotation(std::string(buf)); ///< 添加注解
}

/**
 * @brief 结束Span
 * 
 * 设置结束时间，计算持续时间，并上报Span
 */
void Span::End() {
    // 设置结束时间戳
    end_time_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    duration_us_ = end_time_us_ - start_time_us_; ///< 计算持续时间
    Report(); ///< 上报Span
}

/**
 * @brief 将ID转换为十六进制字符串
 * 
 * @param id 要转换的ID
 * @return std::string 十六进制字符串
 */
static std::string IdToHex(uint64_t id) {
    if (id == 0) return ""; ///< 0返回空字符串
    std::stringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << id; ///< 格式化为16位十六进制字符串
    return ss.str(); ///< 返回字符串
}

/**
 * @brief 获取跟踪ID的十六进制字符串
 * 
 * @return std::string 跟踪ID的十六进制字符串
 */
std::string Span::TraceIdStr() const { return IdToHex(trace_id_); }

/**
 * @brief 获取Span ID的十六进制字符串
 * 
 * @return std::string Span ID的十六进制字符串
 */
std::string Span::SpanIdStr() const { return IdToHex(span_id_); }

/**
 * @brief 获取父Span ID的十六进制字符串
 * 
 * @return std::string 父Span ID的十六进制字符串
 */
std::string Span::ParentSpanIdStr() const { return IdToHex(parent_span_id_); }

/**
 * @brief 将Span转换为JSON对象
 * 
 * @return nlohmann::json JSON对象
 */
nlohmann::json Span::ToJson() const {
    nlohmann::json j; ///< 创建JSON对象
    j["trace_id"] = TraceIdStr(); ///< 跟踪ID
    j["span_id"] = SpanIdStr(); ///< Span ID
    j["parent_span_id"] = ParentSpanIdStr(); ///< 父Span ID
    j["name"] = name_; ///< Span名称
    j["kind"] = (kind_ == SpanKind::CLIENT ? "CLIENT" : (kind_ == SpanKind::SERVER ? "SERVER" : "INTERNAL")); ///< Span类型
    j["start_time_us"] = start_time_us_; ///< 开始时间
    j["duration_us"] = duration_us_; ///< 持续时间
    j["received_real_us"] = received_real_us_; ///< 实际接收时间
    j["start_parse_real_us"] = start_parse_real_us_; ///< 开始解析时间
    j["start_callback_real_us"] = start_callback_real_us_; ///< 开始回调时间
    j["start_send_real_us"] = start_send_real_us_; ///< 开始发送时间
    j["sent_real_us"] = sent_real_us_; ///< 发送完成时间
    j["remote_side"] = remote_side_; ///< 远程地址
    j["request_size"] = request_size_; ///< 请求大小
    j["response_size"] = response_size_; ///< 响应大小
    j["error_code"] = error_code_; ///< 错误码
    
    std::vector<nlohmann::json> anns; ///< 注解JSON数组
    {
        std::lock_guard<std::mutex> lock(mutex_); ///< 加锁保护注解和标签
        for (const auto& ann : annotations_) {
            anns.push_back({{"ts", ann.timestamp_us}, {"val", ann.value}}); ///< 添加注解
        }
        j["annotations"] = anns; ///< 设置注解数组
        j["tags"] = tags_; ///< 设置标签映射
    }
    return j; ///< 返回JSON对象
}

/**
 * @brief 上报Span
 * 
 * 将Span信息输出到日志
 */
void Span::Report() {
    // 生产环境中，这会发送到收集器
    // 目前，仅当日志级别为TRACE时输出
    // 确保包含[TRACE]标签以便测试脚本识别
    LOG_TRACE << ToJson().dump(); ///< 输出JSON格式的Span信息
}

} // namespace tracing
} // namespace mprpc
