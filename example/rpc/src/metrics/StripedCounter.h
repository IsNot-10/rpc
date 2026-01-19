#pragma once

#include <atomic>
#include <vector>
#include <thread>
#include <functional>

namespace metrics {

// =================================================================================
// 高性能计数器 (Striped Long Adder)
// 原理：通过分散热点 (Striping) 减少多线程竞争，类似 Java 的 LongAdder
// =================================================================================
class StripedCounter {
public:
    StripedCounter() {
        for (auto& cell : cells_) {
            cell.store(0, std::memory_order_relaxed);
        }
    }

    void Inc(int64_t v = 1) {
        // 根据线程 ID 哈希映射到某个 Cell
        size_t h = std::hash<std::thread::id>{}(std::this_thread::get_id());
        size_t idx = h & (kNumCells - 1);
        cells_[idx].fetch_add(v, std::memory_order_relaxed);
    }

    int64_t Value() const {
        int64_t sum = 0;
        for (const auto& cell : cells_) {
            sum += cell.load(std::memory_order_relaxed);
        }
        return sum;
    }

    void Reset() {
        for (auto& cell : cells_) {
            cell.store(0, std::memory_order_relaxed);
        }
    }

private:
    static const size_t kNumCells = 32;
    // 使用 alignas(64) 避免伪共享 (False Sharing)
    struct alignas(64) PaddedAtomic : public std::atomic<int64_t> {
        using std::atomic<int64_t>::atomic;
    };
    PaddedAtomic cells_[kNumCells];
};

} // namespace metrics
