#pragma once
#include <string>
#include <map>
#include <mutex>
#include <atomic>
#include <memory>
#include <shared_mutex>

namespace ha {

// Concurrency Limiter
// Manages in-flight requests per host.
// Uses shared_mutex for map access and atomic counters for values to improve concurrency.
class ConcurrencyLimiter {
public:
    static ConcurrencyLimiter& instance();
    
    // Increase in-flight count for host
    void inc(const std::string& host);
    
    // Decrease in-flight count for host
    void dec(const std::string& host);
    
    // Get current in-flight count for host
    int get(const std::string& host);

    // Set global max concurrency limit (0 means no limit)
    void set_max_concurrency(int limit);
    
    // Check if new request is allowed
    bool is_allowed(const std::string& host);

private:
    // Helper to get or create counter
    std::shared_ptr<std::atomic<int>> get_counter(const std::string& host);

    std::map<std::string, std::shared_ptr<std::atomic<int>>> inflight_map_;
    std::shared_mutex mutex_; 
    std::atomic<int> max_concurrency_{0};
};

} // namespace ha
