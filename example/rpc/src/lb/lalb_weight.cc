#include "lb/lalb_weight.h"
#include "lb/lb_common.h"
#include <algorithm>
#include <cmath>

using namespace lb;

LalbWeight::LalbWeight(int64_t initial_weight)
    : _weight(initial_weight), _base_weight(initial_weight),
      _begin_time_sum(0), _begin_time_count(0),
      _old_diff_sum(0), _old_index((size_t)-1),
      _avg_latency(0), _total_latency(0) {
    _time_q.resize(RECV_QUEUE_SIZE);
}

void LalbWeight::Seed(double avg_latency, int64_t weight_scale, int64_t min_weight) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (avg_latency > 0) {
        _avg_latency = (int64_t)avg_latency;
        // 限制延迟下限，避免对于极快本地调用产生极端权重
        int64_t safe_latency = std::max<int64_t>(_avg_latency, MIN_LATENCY_US); 
        _base_weight = weight_scale / safe_latency;
        if (_base_weight < min_weight) _base_weight = min_weight;
        _weight = _base_weight;
    }
}

LalbWeight::AddInflightResult LalbWeight::AddInflight(size_t index, int64_t dice, int64_t left, int64_t now_us, double punish_inflight_ratio, int64_t min_weight) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (_base_weight < 0) {
        return {false, 0};
    }
    const int64_t diff = ResetWeight(index, now_us, punish_inflight_ratio, min_weight);
    if (_weight < (dice - left)) {
        return {false, diff};
    }
    _begin_time_sum += now_us;
    ++_begin_time_count;
    return {true, diff};
}

int64_t LalbWeight::MarkFailed(size_t index, int64_t avg_weight) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (_base_weight <= avg_weight) {
        return 0;
    }
    _base_weight = avg_weight;
    return ResetWeight(index, 0, 0, 0);
}

int64_t LalbWeight::Update(bool success, int64_t begin_time_us, int64_t end_time_us, int64_t timeout_ms, int retried_count, double punish_error_ratio, int64_t weight_scale, int64_t min_weight, double punish_inflight_ratio, size_t index) {
    int64_t latency = end_time_us - begin_time_us;
    if (latency <= 0) latency = 1;

    std::lock_guard<std::mutex> lk(_mutex);
    if (_base_weight < 0) {
        return 0;
    }
    _begin_time_sum -= begin_time_us;
    --_begin_time_count;
    if (_begin_time_count < 0) {
        _begin_time_count = 0;
        _begin_time_sum = 0;
    }

    int64_t effective_latency = latency;
    if (!success) {
        effective_latency = (int64_t)(latency * punish_error_ratio);
        if (timeout_ms > 0) {
            effective_latency = std::max(effective_latency, timeout_ms * 1000L);
        }
    }

    // Add to circular buffer
    TimeInfo tm_info = { effective_latency, end_time_us };
    
    if (_time_q_count < RECV_QUEUE_SIZE) {
        size_t new_idx = (_time_q_start + _time_q_count) % RECV_QUEUE_SIZE;
        _time_q[new_idx] = tm_info;
        _time_q_count++;
        _total_latency += effective_latency;
    } else {
        // Buffer full, overwrite oldest (top)
        _total_latency -= _time_q[_time_q_start].latency;
        _time_q[_time_q_start] = tm_info;
        _time_q_start = (_time_q_start + 1) % RECV_QUEUE_SIZE;
        _total_latency += effective_latency;
    }

    // Calculate QPS and Avg Latency
    if (_time_q_count > 1) {
        size_t top_idx = _time_q_start;
        int64_t top_time_us = _time_q[top_idx].end_time_us;
        
        int64_t scaled_qps = DEFAULT_QPS * weight_scale;
        int64_t time_range = end_time_us - top_time_us;
        
        // QPS calculation: (count - 1) requests in (end_time - top_time) duration
        if (time_range > 0) {
             scaled_qps = (_time_q_count - 1) * 1000000L * weight_scale / time_range;
             if (scaled_qps < weight_scale) scaled_qps = weight_scale;
        }
        
        _avg_latency = _total_latency / _time_q_count;
        
        if (_avg_latency > 0) {
            int64_t safe_latency = std::max<int64_t>(_avg_latency, MIN_LATENCY_US);
            // Disable QPS-based positive feedback loop for stability test
            // _base_weight = scaled_qps / safe_latency;
            _base_weight = weight_scale / safe_latency;
        }
    } else if (_time_q_count == 1) {
        _avg_latency = _total_latency;
    }

    if (_base_weight < min_weight) _base_weight = min_weight;
    
    return ResetWeight(index, end_time_us, punish_inflight_ratio, min_weight);
}

void LalbWeight::SetWeight(int64_t weight) {
    std::lock_guard<std::mutex> lk(_mutex);
    _base_weight = weight;
    _weight = weight;
}

int64_t LalbWeight::Disable() {
    std::lock_guard<std::mutex> lk(_mutex);
    const int64_t saved = _weight;
    _base_weight = -1;
    _weight = 0;
    return saved;
}

int64_t LalbWeight::Value() const { 
    return _weight; 
}

int64_t LalbWeight::AvgLatency() const { return _avg_latency; }

int64_t LalbWeight::ResetWeight(size_t index, int64_t now_us, double punish_inflight_ratio, int64_t min_weight) {
    int64_t new_weight = _base_weight;
    if (_begin_time_count > 0 && _avg_latency > 0) {
        const int64_t inflight_delay = now_us - _begin_time_sum / _begin_time_count;
        int64_t safe_latency = std::max<int64_t>(_avg_latency, MIN_LATENCY_US);
        const int64_t punish_latency = (int64_t)(safe_latency * punish_inflight_ratio);
        if (inflight_delay >= punish_latency) {
            new_weight = new_weight * punish_latency / std::max<int64_t>(1, inflight_delay);
        }
    }
    if (new_weight < min_weight) {
        new_weight = min_weight;
    }
    const int64_t old_weight = _weight;
    _weight = new_weight;
    const int64_t diff = new_weight - old_weight;
    if (_old_index == index && diff != 0) {
        _old_diff_sum += diff;
    }
    return diff;
}
