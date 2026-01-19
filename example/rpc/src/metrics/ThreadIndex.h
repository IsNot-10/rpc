#pragma once

#include <atomic>

namespace metrics {

class ThreadIndex {
public:
    static int Get() {
        // 使用 static thread_local 初始化机制，消除每次调用的 if 判断分支
        // 只有在第一次访问时会调用 Assign()
        static thread_local int idx = Assign();
        return idx;
    }
    
    static int Max() {
        return next_index_.load(std::memory_order_relaxed);
    }

private:
    static int Assign() {
        int id = next_index_.fetch_add(1, std::memory_order_relaxed);
        // 如果线程数超过限制，回绕到 0 (简单的 fallback，防止越界)
        if (id >= 2048) { // 假设 kMaxThreads 是 2048
            return id % 2048; 
        }
        return id;
    }

    static std::atomic<int> next_index_;
};

} // namespace metrics
