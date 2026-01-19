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

static thread_local std::shared_ptr<Span> t_current_span;

struct IdGen {
    bool init;
    uint16_t seq;
    uint64_t current_random;
    std::mt19937_64 generator;
    std::uniform_int_distribution<uint64_t> distribution;
};

static thread_local IdGen t_id_gen = { false, 0, 0 };

static void InitIdGen() {
    if (!t_id_gen.init) {
        t_id_gen.init = true;
        // Seed with thread id + time
        uint64_t seed = std::hash<std::thread::id>()(std::this_thread::get_id()) + 
                        std::chrono::system_clock::now().time_since_epoch().count();
        t_id_gen.generator.seed(seed);
        t_id_gen.current_random = t_id_gen.distribution(t_id_gen.generator);
    }
}

uint64_t TraceContext::GenerateId() {
    InitIdGen();
    if (t_id_gen.seq == 0) {
        t_id_gen.current_random = t_id_gen.distribution(t_id_gen.generator);
        t_id_gen.seq = 1;
    }
    // Mix random high bits with sequence low bits to avoid collision in same thread
    return (t_id_gen.current_random & 0xFFFFFFFFFFFF0000ULL) | (t_id_gen.seq++);
}

void TraceContext::Clear() {
    t_current_span.reset();
}

TraceContext::ContextSnapshot TraceContext::GetSnapshot() {
    return {t_current_span};
}

void TraceContext::RestoreSnapshot(const ContextSnapshot& snapshot) {
    t_current_span = snapshot.span;
}

std::shared_ptr<Span> TraceContext::GetCurrentSpan() {
    return t_current_span;
}

void TraceContext::SetCurrentSpan(std::shared_ptr<Span> span) {
    t_current_span = span;
}

// ------------------------------------------------------------------
// Span Implementation
// ------------------------------------------------------------------

Span::Span(uint64_t trace_id, uint64_t span_id, uint64_t parent_span_id, 
           const std::string& name, SpanKind kind)
    : trace_id_(trace_id), span_id_(span_id), parent_span_id_(parent_span_id),
      name_(name), kind_(kind) {
    start_time_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::shared_ptr<Span> Span::CreateClientSpan(const std::string& name) {
    uint64_t trace_id;
    uint64_t parent_span_id = 0;
    
    auto parent = TraceContext::GetCurrentSpan();
    if (parent) {
        trace_id = parent->trace_id();
        parent_span_id = parent->span_id();
    } else {
        trace_id = TraceContext::GenerateId();
    }
    
    uint64_t span_id = TraceContext::GenerateId();
    return std::make_shared<Span>(trace_id, span_id, parent_span_id, name, SpanKind::CLIENT);
}

static uint64_t ParseId(const std::string& str) {
    if (str.empty()) return 0;
    try {
        return std::stoull(str, nullptr, 16); // Parse as hex
    } catch (...) {
        return 0;
    }
}

std::shared_ptr<Span> Span::CreateServerSpan(const std::string& trace_id_str, const std::string& span_id_str, 
                                            const std::string& parent_span_id_str, const std::string& name) {
    uint64_t trace_id = ParseId(trace_id_str);
    uint64_t parent_span_id = ParseId(parent_span_id_str); // The caller's span_id is our parent
    
    // If no trace_id (root request), generate one
    if (trace_id == 0) {
        trace_id = TraceContext::GenerateId();
    }

    // We generate a NEW span_id for ourselves
    uint64_t my_span_id = TraceContext::GenerateId();
    
    return std::make_shared<Span>(trace_id, my_span_id, parent_span_id, name, SpanKind::SERVER);
}

std::shared_ptr<Span> Span::CreateServerSpan(uint64_t trace_id, uint64_t span_id, uint64_t parent_span_id, 
                                            const std::string& name) {
    return std::make_shared<Span>(trace_id, span_id, parent_span_id, name, SpanKind::SERVER);
}

void Span::AddAnnotation(const std::string& value) {
    int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(mutex_);
    annotations_.push_back({now, value});
}

void Span::Annotate(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    AddAnnotation(std::string(buf));
}

void Span::End() {
    end_time_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    duration_us_ = end_time_us_ - start_time_us_;
    Report();
}

static std::string IdToHex(uint64_t id) {
    if (id == 0) return "";
    std::stringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << id;
    return ss.str();
}

std::string Span::TraceIdStr() const { return IdToHex(trace_id_); }
std::string Span::SpanIdStr() const { return IdToHex(span_id_); }
std::string Span::ParentSpanIdStr() const { return IdToHex(parent_span_id_); }

nlohmann::json Span::ToJson() const {
    nlohmann::json j;
    j["trace_id"] = TraceIdStr();
    j["span_id"] = SpanIdStr();
    j["parent_span_id"] = ParentSpanIdStr();
    j["name"] = name_;
    j["kind"] = (kind_ == SpanKind::CLIENT ? "CLIENT" : (kind_ == SpanKind::SERVER ? "SERVER" : "INTERNAL"));
    j["start_time_us"] = start_time_us_;
    j["duration_us"] = duration_us_;
    j["received_real_us"] = received_real_us_;
    j["start_parse_real_us"] = start_parse_real_us_;
    j["start_callback_real_us"] = start_callback_real_us_;
    j["start_send_real_us"] = start_send_real_us_;
    j["sent_real_us"] = sent_real_us_;
    j["remote_side"] = remote_side_;
    j["request_size"] = request_size_;
    j["response_size"] = response_size_;
    j["error_code"] = error_code_;
    
    std::vector<nlohmann::json> anns;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& ann : annotations_) {
            anns.push_back({{"ts", ann.timestamp_us}, {"val", ann.value}});
        }
        j["annotations"] = anns;
        j["tags"] = tags_;
    }
    return j;
}

void Span::Report() {
    // In production, this would go to a collector.
    // For now, log if trace level is enabled
    // Ensure [TRACE] tag is present for test script
    LOG_TRACE << ToJson().dump();
}

} // namespace tracing
} // namespace mprpc
