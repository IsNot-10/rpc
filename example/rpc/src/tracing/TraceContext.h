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
namespace tracing {

enum class SpanKind {
    CLIENT,
    SERVER,
    INTERNAL
};

struct Annotation {
    int64_t timestamp_us;
    std::string value;
};

class Span {
public:
    Span(uint64_t trace_id, uint64_t span_id, uint64_t parent_span_id, 
         const std::string& name, SpanKind kind);
    ~Span() = default;

    // Factory methods matching BRPC style
    static std::shared_ptr<Span> CreateClientSpan(const std::string& name);
    static std::shared_ptr<Span> CreateServerSpan(const std::string& trace_id_str, const std::string& span_id_str, 
                                                const std::string& parent_span_id_str, const std::string& name);
    static std::shared_ptr<Span> CreateServerSpan(uint64_t trace_id, uint64_t span_id, uint64_t parent_span_id, 
                                                const std::string& name);

    void AddAnnotation(const std::string& value);
    void Annotate(const char* fmt, ...);
    
    // Setters for BRPC-like metrics
    void SetRemoteSide(const std::string& addr) { remote_side_ = addr; }
    void SetRequestSize(int size) { request_size_ = size; }
    void SetResponseSize(int size) { response_size_ = size; }
    void SetErrorCode(int code) { error_code_ = code; }

    // Fine-grained timestamps (BRPC style)
    void SetReceivedRealUs(int64_t t) { received_real_us_ = t; }
    void SetStartParseRealUs(int64_t t) { start_parse_real_us_ = t; }
    void SetStartCallbackRealUs(int64_t t) { start_callback_real_us_ = t; }
    void SetStartSendRealUs(int64_t t) { start_send_real_us_ = t; }
    void SetSentRealUs(int64_t t) { sent_real_us_ = t; }
    
    // Add generic tag
    void AddTag(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        tags_[key] = value;
    }

    void End();

    // Getters
    uint64_t trace_id() const { return trace_id_; }
    uint64_t span_id() const { return span_id_; }
    uint64_t parent_span_id() const { return parent_span_id_; }
    
    std::string TraceIdStr() const;
    std::string SpanIdStr() const;
    std::string ParentSpanIdStr() const;

    nlohmann::json ToJson() const;

private:
    void Report(); // Output to log

    uint64_t trace_id_;
    uint64_t span_id_;
    uint64_t parent_span_id_;
    
    std::string name_; // e.g., Service.Method
    SpanKind kind_;
    
    int64_t start_time_us_;
    int64_t end_time_us_ = 0;
    int64_t duration_us_ = 0;
    
    // BRPC style timestamps
    int64_t received_real_us_ = 0;
    int64_t start_parse_real_us_ = 0;
    int64_t start_callback_real_us_ = 0;
    int64_t start_send_real_us_ = 0;
    int64_t sent_real_us_ = 0;

    std::string remote_side_;
    int request_size_ = 0;
    int response_size_ = 0;
    int error_code_ = 0;

    std::vector<Annotation> annotations_;
    std::map<std::string, std::string> tags_;
    mutable std::mutex mutex_;
};

class TraceContext {
public:
    // Generate a new uint64 ID
    static uint64_t GenerateId();

    // Clear current thread's trace info
    static void Clear();

    // Context snapshot for passing across threads
    struct ContextSnapshot {
        std::shared_ptr<Span> span;
    };

    static ContextSnapshot GetSnapshot();
    static void RestoreSnapshot(const ContextSnapshot& snapshot);

    static std::shared_ptr<Span> GetCurrentSpan();
    static void SetCurrentSpan(std::shared_ptr<Span> span);

    // Helper for TRACEPRINTF
    template<typename... Args>
    static void TracePrintf(const char* fmt, Args... args) {
        auto span = GetCurrentSpan();
        if (span) {
            span->Annotate(fmt, args...);
        }
    }

private:
    static thread_local std::shared_ptr<Span> current_span_;
};

} // namespace tracing
} // namespace mprpc

#define TRACEPRINTF(fmt, ...) \
    mprpc::tracing::TraceContext::TracePrintf(fmt, ##__VA_ARGS__)
