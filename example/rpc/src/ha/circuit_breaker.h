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

// EmaErrorRecorder adapted from BRPC
// Records error rate and latency using Exponential Moving Average (EMA)
class EmaErrorRecorder {
public:
    EmaErrorRecorder(int window_size, int max_error_percent);
    
    // Returns true if healthy, false if unhealthy
    bool on_call_end(int error_code, int64_t latency);
    void reset();
    
    double get_error_rate() const;

private:
    int64_t update_latency(int64_t latency);
    bool update_error_cost(int64_t latency, int64_t ema_latency);

    const int window_size_;
    const int max_error_percent_;
    const double smooth_;
    
    std::atomic<int32_t> sample_count_when_initializing_{0};
    std::atomic<int32_t> error_count_when_initializing_{0};
    std::atomic<int64_t> ema_error_cost_{0};
    std::atomic<int64_t> ema_latency_{0};
};

class CircuitBreakerNode {
public:
    CircuitBreakerNode();
    ~CircuitBreakerNode() = default;

    // Returns false if the node should be isolated
    bool on_call_end(int error_code, int64_t latency);
    
    // Reset internal state
    void reset();
    
    // Mark as broken explicitly
    void mark_as_broken();
    
    // Check if the node is currently available (not isolated)
    bool is_available();
    
    // Get isolation duration
    int isolation_duration_ms() const {
        return isolation_duration_ms_.load(std::memory_order_relaxed);
    }
    
    // Get number of times isolated
    int isolated_times() const {
        return isolated_times_.load(std::memory_order_relaxed);
    }

private:
    void update_isolation_duration();

    EmaErrorRecorder long_window_;
    EmaErrorRecorder short_window_;
    
    int64_t last_reset_time_ms_{0};
    std::atomic<int> isolation_duration_ms_;
    std::atomic<int> isolated_times_{0};
    std::atomic<bool> broken_{false};
    
    // Time when the node will be available again (if broken)
    std::chrono::steady_clock::time_point broken_until_;
    std::mutex mutex_; // Protects broken_until_ updates during state transitions
};

class CircuitBreaker {
public:
    static CircuitBreaker& instance();
    
    // Check if host is accessible
    bool should_access(const std::string& host);
    
    // Report request status
    // error_code: 0 for success, non-zero for failure
    void report_status(const std::string& host, int error_code, int64_t latency_ms);
    
    // Helper wrapper for boolean success status
    void report_status(const std::string& host, bool success, int64_t latency_ms);
    
    // Get estimated latency (from short window or similar)
    // Note: BRPC's EmaErrorRecorder stores ema_latency inside.
    // We'll expose it via a helper if needed, or just return -1 for now if not critical.
    double get_latency(const std::string& host);

    void reset(const std::string& host);

private:
    std::shared_ptr<CircuitBreakerNode> get_node(const std::string& host);

    std::map<std::string, std::shared_ptr<CircuitBreakerNode>> nodes_;
    std::shared_mutex mutex_;
};

} // namespace ha
