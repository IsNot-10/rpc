#include "ha/concurrency_limiter.h"

namespace ha {

ConcurrencyLimiter& ConcurrencyLimiter::instance() {
    static ConcurrencyLimiter inst;
    return inst;
}

std::shared_ptr<std::atomic<int>> ConcurrencyLimiter::get_counter(const std::string& host) {
    // Fast path: read lock
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = inflight_map_.find(host);
        if (it != inflight_map_.end()) {
            return it->second;
        }
    }
    
    // Slow path: write lock
    std::unique_lock<std::shared_mutex> lock(mutex_);
    // Double check
    auto it = inflight_map_.find(host);
    if (it != inflight_map_.end()) {
        return it->second;
    }
    
    auto counter = std::make_shared<std::atomic<int>>(0);
    inflight_map_[host] = counter;
    return counter;
}

void ConcurrencyLimiter::inc(const std::string& host) {
    auto counter = get_counter(host);
    counter->fetch_add(1, std::memory_order_relaxed);
}

void ConcurrencyLimiter::dec(const std::string& host) {
    auto counter = get_counter(host);
    // Prevent negative
    int old_val = counter->load(std::memory_order_relaxed);
    while (old_val > 0) {
        if (counter->compare_exchange_weak(old_val, old_val - 1, std::memory_order_relaxed)) {
            break;
        }
    }
}

int ConcurrencyLimiter::get(const std::string& host) {
    auto counter = get_counter(host);
    return counter->load(std::memory_order_relaxed);
}

void ConcurrencyLimiter::set_max_concurrency(int limit) {
    max_concurrency_.store(limit, std::memory_order_relaxed);
}

bool ConcurrencyLimiter::is_allowed(const std::string& host) {
    int max = max_concurrency_.load(std::memory_order_relaxed);
    if (max <= 0) return true;
    
    auto counter = get_counter(host);
    return counter->load(std::memory_order_relaxed) < max;
}

} // namespace ha
