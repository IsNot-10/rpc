#include "ha/circuit_breaker.h"
#include "Logging.h"
#include <cmath>
#include <iostream>

namespace ha {

namespace {
    // Configuration constants (adapted from BRPC defaults)
    const int SHORT_WINDOW_SIZE = 1500;
    const int LONG_WINDOW_SIZE = 3000;
    const int SHORT_WINDOW_ERROR_PERCENT = 10;
    const int LONG_WINDOW_ERROR_PERCENT = 5;
    const int MIN_ISOLATION_DURATION_MS = 100;
    const int MAX_ISOLATION_DURATION_MS = 30000;
    const double EPSILON_VALUE = 0.02;
    const int MIN_ERROR_COST_US = 500;
    const int MAX_FAILED_LATENCY_MULTIPLE = 2;
}

EmaErrorRecorder::EmaErrorRecorder(int window_size, int max_error_percent)
    : window_size_(window_size)
    , max_error_percent_(max_error_percent)
    , smooth_(std::pow(EPSILON_VALUE, 1.0 / window_size))
{
}

bool EmaErrorRecorder::on_call_end(int error_code, int64_t latency) {
    int64_t ema_latency = 0;
    bool healthy = false;
    
    if (error_code == 0) {
        ema_latency = update_latency(latency);
        healthy = update_error_cost(0, ema_latency);
    } else {
        ema_latency = ema_latency_.load(std::memory_order_relaxed);
        healthy = update_error_cost(latency, ema_latency);
    }

    // Initialization phase
    if (sample_count_when_initializing_.load(std::memory_order_relaxed) < window_size_) {
        int32_t count = sample_count_when_initializing_.fetch_add(1, std::memory_order_relaxed);
        if (count < window_size_) {
            if (error_code != 0) {
                int32_t error_count = error_count_when_initializing_.fetch_add(1, std::memory_order_relaxed);
                return error_count < (window_size_ * max_error_percent_ / 100);
            }
            return true;
        }
    }

    return healthy;
}

void EmaErrorRecorder::reset() {
    if (sample_count_when_initializing_.load(std::memory_order_relaxed) < window_size_) {
        sample_count_when_initializing_.store(0, std::memory_order_relaxed);
        error_count_when_initializing_.store(0, std::memory_order_relaxed);
        ema_latency_.store(0, std::memory_order_relaxed);
    }
    ema_error_cost_.store(0, std::memory_order_relaxed);
}

double EmaErrorRecorder::get_error_rate() const {
    // Approximate error rate for display
    return 0.0; // Not easily calculable from EMA cost/latency without more state
}

int64_t EmaErrorRecorder::update_latency(int64_t latency) {
    int64_t ema_latency = ema_latency_.load(std::memory_order_relaxed);
    do {
        int64_t next_ema_latency = 0;
        if (ema_latency == 0) {
            next_ema_latency = latency;
        } else {
            next_ema_latency = ema_latency * smooth_ + latency * (1.0 - smooth_);
        }
        if (ema_latency_.compare_exchange_weak(ema_latency, next_ema_latency)) {
            return next_ema_latency;
        }
    } while (true);
}

bool EmaErrorRecorder::update_error_cost(int64_t error_cost, int64_t ema_latency) {
    if (ema_latency != 0) {
        // Cap the error cost
        error_cost = std::min(ema_latency * MAX_FAILED_LATENCY_MULTIPLE, error_cost);
    }

    // Error response
    if (error_cost != 0) {
        int64_t current_ema_error_cost = ema_error_cost_.fetch_add(error_cost, std::memory_order_relaxed);
        current_ema_error_cost += error_cost;
        
        double max_error_cost = ema_latency * window_size_ * (max_error_percent_ / 100.0) * (1.0 + EPSILON_VALUE);
        return current_ema_error_cost <= max_error_cost;
    }

    // Success response
    int64_t ema_error_cost = ema_error_cost_.load(std::memory_order_relaxed);
    do {
        if (ema_error_cost == 0) break;
        
        if (ema_error_cost < MIN_ERROR_COST_US) {
            if (ema_error_cost_.compare_exchange_weak(ema_error_cost, 0, std::memory_order_relaxed)) {
                break;
            }
        } else {
            int64_t next_ema_error_cost = ema_error_cost * smooth_;
            if (ema_error_cost_.compare_exchange_weak(ema_error_cost, next_ema_error_cost)) {
                break;
            }
        }
    } while (true);
    
    return true;
}

// CircuitBreakerNode Implementation

CircuitBreakerNode::CircuitBreakerNode()
    : long_window_(LONG_WINDOW_SIZE, LONG_WINDOW_ERROR_PERCENT)
    , short_window_(SHORT_WINDOW_SIZE, SHORT_WINDOW_ERROR_PERCENT)
    , isolation_duration_ms_(MIN_ISOLATION_DURATION_MS)
{
}

bool CircuitBreakerNode::on_call_end(int error_code, int64_t latency) {
    // If broken, return false (isolate)
    if (broken_.load(std::memory_order_relaxed)) {
        return false;
    }

    bool long_healthy = long_window_.on_call_end(error_code, latency);
    bool short_healthy = short_window_.on_call_end(error_code, latency);

    if (long_healthy && short_healthy) {
        return true;
    }

    mark_as_broken();
    return false;
}

void CircuitBreakerNode::reset() {
    long_window_.reset();
    short_window_.reset();
    last_reset_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    broken_.store(false, std::memory_order_release);
}

void CircuitBreakerNode::mark_as_broken() {
    bool expected = false;
    if (broken_.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        isolated_times_.fetch_add(1, std::memory_order_relaxed);
        update_isolation_duration();
        
        // Set broken_until time
        std::lock_guard<std::mutex> lock(mutex_);
        broken_until_ = std::chrono::steady_clock::now() + 
                       std::chrono::milliseconds(isolation_duration_ms_.load(std::memory_order_relaxed));
        
        LOG_WARN << "Node marked as BROKEN. Isolation duration: " << isolation_duration_ms_.load() << "ms";
    }
}

void CircuitBreakerNode::update_isolation_duration() {
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    int duration = isolation_duration_ms_.load(std::memory_order_relaxed);
    
    if (now - last_reset_time_ms_ < MAX_ISOLATION_DURATION_MS) {
        duration = std::min(duration * 2, MAX_ISOLATION_DURATION_MS);
    } else {
        duration = MIN_ISOLATION_DURATION_MS;
    }
    
    isolation_duration_ms_.store(duration, std::memory_order_relaxed);
}

bool CircuitBreakerNode::is_available() {
    if (!broken_.load(std::memory_order_relaxed)) {
        return true;
    }
    
    // Check if isolation duration has passed (Half-Open)
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    if (now > broken_until_) {
        return true; // Allow probe
    }
    
    return false;
}

// CircuitBreaker Implementation

CircuitBreaker& CircuitBreaker::instance() {
    static CircuitBreaker inst;
    return inst;
}

std::shared_ptr<CircuitBreakerNode> CircuitBreaker::get_node(const std::string& host) {
    // Fast path: read lock
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = nodes_.find(host);
        if (it != nodes_.end()) {
            return it->second;
        }
    }
    
    // Slow path: write lock
    std::unique_lock<std::shared_mutex> lock(mutex_);
    // Double check
    auto it = nodes_.find(host);
    if (it != nodes_.end()) {
        return it->second;
    }
    
    auto node = std::make_shared<CircuitBreakerNode>();
    nodes_[host] = node;
    return node;
}

bool CircuitBreaker::should_access(const std::string& host) {
    auto node = get_node(host);
    return node->is_available();
}

void CircuitBreaker::report_status(const std::string& host, int error_code, int64_t latency_ms) {
    auto node = get_node(host);
    int64_t latency_us = latency_ms * 1000;
    
    // BRPC logic adaptation:
    // If the node is broken, on_call_end returns false immediately.
    // If we are in half-open state (broken but allowing 1 probe), and the probe succeeds:
    // on_call_end will return false (because it's broken).
    // But since it's a success, we should RECOVER (Reset).
    
    bool healthy = node->on_call_end(error_code, latency_us);
    
    if (error_code == 0) {
        if (!healthy) {
            // Node is broken, but this request succeeded.
            // This means it was a successful probe.
            node->reset();
            LOG_INFO << "CircuitBreaker: Host " << host << " RECOVERED (Probe Success).";
        }
    }
    // If failure, and !healthy, it means we are broken (or just broke).
    // We stay broken.
}

void CircuitBreaker::report_status(const std::string& host, bool success, int64_t latency_ms) {
    report_status(host, success ? 0 : -1, latency_ms);
}

double CircuitBreaker::get_latency(const std::string& host) {
    // Not implemented in EmaErrorRecorder port yet
    return -1.0;
}

void CircuitBreaker::reset(const std::string& host) {
    auto node = get_node(host);
    node->reset();
}

} // namespace ha
