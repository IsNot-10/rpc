#pragma once

#include <string>
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include "LatencyRecorder.h"
#include "StripedCounter.h"

#include <deque>
#include <utility>
#include <sstream>
#include <chrono>
#include <thread>
#include "json.h"

namespace metrics {

using json = nlohmann::json;

// 计数器 (单调递增)
class Counter {
public:
    void Inc(double v = 1.0) { 
        double current = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(current, current + v, std::memory_order_release, std::memory_order_relaxed));
    }
    double Value() const { return value_.load(std::memory_order_acquire); }
    
    std::string GetSeriesJson() const { return "[]"; } 
private:
    std::atomic<double> value_{0.0};
};

// 仪表盘 (可增可减)
class Gauge {
public:
    void Set(double v) { 
        double current = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(current, v, std::memory_order_release, std::memory_order_relaxed));
    }
    void Inc(double v = 1.0) { 
        double current = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(current, current + v, std::memory_order_release, std::memory_order_relaxed));
    }
    void Dec(double v = 1.0) { Inc(-v); }
    double Value() const { return value_.load(std::memory_order_acquire); }
    std::string GetSeriesJson() const { return "[]"; }
private:
    std::atomic<double> value_{0.0};
};

// 简单的线程安全时间序列缓冲区
struct TimeSeries {
    std::deque<std::pair<int64_t, double>> data;
    mutable std::mutex mutex;

    void Add(int64_t ts, double val) {
        std::lock_guard<std::mutex> lock(mutex);
        data.push_back({ts, val});
        // 保留最近 60 个点 (假设每秒一次，即 1 分钟)
        if (data.size() > 60) {
            data.pop_front();
        }
    }

    json ToJson() const {
        std::lock_guard<std::mutex> lock(mutex);
        json j = json::array();
        for (const auto& p : data) {
            j.push_back({p.first, p.second});
        }
        return j;
    }
};

// 直方图 (用于统计延迟分布、QPS 等)
class Histogram {
public:
    Histogram() {
        last_tick_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // 记录观测值 (秒)
    void Observe(double val) {
        recorder_.Record((int64_t)(val * 1000000)); // 转为微秒
    }
    
    std::vector<int64_t> GetCounts() { return recorder_.GetCounts(); }
    const std::vector<int64_t>& GetBuckets() const { return recorder_.GetBuckets(); }
    double GetSum() { return recorder_.GetAvgLatency() * recorder_.GetCount(); } // 近似值
    int64_t GetCount() { return recorder_.GetCount(); }

    double GetQPS() { 
        std::lock_guard<std::mutex> lock(mutex_);
        return last_qps_; 
    }
    
    int64_t GetMaxLatency() { return recorder_.GetMaxLatency(); }
    double GetAvgLatency() { return recorder_.GetAvgLatency(); }

    // 获取序列数据 (JSON String)
    // 格式: [{"label": "QPS", "data": [[ts, val], ...]}, ...]
    std::string GetSeriesJson() {
        json j = json::array();
        
        {
            json item;
            item["label"] = "QPS";
            item["data"] = qps_series_.ToJson();
            j.push_back(item);
        }
        {
            json item;
            item["label"] = "Max Latency";
            item["data"] = max_lat_series_.ToJson();
            item["yaxis"] = 2;
            j.push_back(item);
        }
        {
            json item;
            item["label"] = "Avg Latency";
            item["data"] = avg_lat_series_.ToJson();
            item["yaxis"] = 2;
            j.push_back(item);
        }

        return j.dump();
    }
    
    // 定时更新 QPS 和历史数据
    void Tick() {
        auto now_steady = std::chrono::steady_clock::now();
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_steady.time_since_epoch()).count();
        
        auto now_system = std::chrono::system_clock::now();
        int64_t now_ts = std::chrono::duration_cast<std::chrono::milliseconds>(now_system.time_since_epoch()).count();
            
        int64_t current_count = recorder_.GetCount();
        double current_max_lat = (double)recorder_.GetMaxLatency() / 1000000.0; // s
        double current_avg_lat = recorder_.GetAvgLatency() / 1000000.0; // s

        std::lock_guard<std::mutex> lock(mutex_);
        
        if (last_tick_time_ > 0) {
            double delta_sec = (now_ms - last_tick_time_) / 1000.0;
            if (delta_sec > 0) {
                last_qps_ = (current_count - last_count_) / delta_sec;
                if (last_qps_ < 0) last_qps_ = 0;
            }
        }
        
        // 记录历史数据
        qps_series_.Add(now_ts, last_qps_);
        max_lat_series_.Add(now_ts, current_max_lat);
        avg_lat_series_.Add(now_ts, current_avg_lat);

        last_count_ = current_count;
        last_tick_time_ = now_ms;
    }

private:
    LatencyRecorder recorder_;
    std::mutex mutex_;
    int64_t last_tick_time_ = 0;
    int64_t last_count_ = 0;
    double last_qps_ = 0.0;

    TimeSeries qps_series_;
    TimeSeries max_lat_series_;
    TimeSeries avg_lat_series_;
};

struct MetricMeta {
    std::string name;
    std::string help;
    std::map<std::string, std::string> labels;
    std::string type;
};

class MetricsRegistry {
public:
    static MetricsRegistry& instance();

    std::shared_ptr<Counter> GetCounter(const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels);
    std::shared_ptr<Gauge> GetGauge(const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels);
    std::shared_ptr<Histogram> GetHistogram(const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels);

    std::string ToPrometheus();
    
    // 返回用于前端展示的 JSON
    std::string ToJson() {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        json j;
        j["metrics"] = json::array();

        for (const auto& kv : counters_) {
            json item;
            item["id"] = kv.first;
            item["name"] = metas_[kv.first].name;
            item["help"] = metas_[kv.first].help;
            item["labels"] = metas_[kv.first].labels;
            item["type"] = "counter";
            item["value"] = kv.second->Value();
            j["metrics"].push_back(item);
        }

        for (const auto& kv : gauges_) {
            json item;
            item["id"] = kv.first;
            item["name"] = metas_[kv.first].name;
            item["help"] = metas_[kv.first].help;
            item["labels"] = metas_[kv.first].labels;
            item["type"] = "gauge";
            item["value"] = kv.second->Value();
            j["metrics"].push_back(item);
        }

        for (const auto& kv : histograms_) {
            json item;
            item["id"] = kv.first;
            item["name"] = metas_[kv.first].name;
            item["help"] = metas_[kv.first].help;
            item["labels"] = metas_[kv.first].labels;
            item["type"] = "histogram";
            item["qps"] = kv.second->GetQPS();
            item["max_latency"] = (double)kv.second->GetMaxLatency() / 1000000.0;
            item["avg_latency"] = kv.second->GetAvgLatency() / 1000000.0;
            item["count"] = kv.second->GetCount();
            j["metrics"].push_back(item);
        }
        
        return j.dump();
    }

    std::string GetSeriesJson(const std::string& name) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (histograms_.find(name) != histograms_.end()) {
            return histograms_[name]->GetSeriesJson();
        }
        return "[]";
    }

    void StartTicker() {
        std::thread([this](){
            while(true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::shared_lock<std::shared_mutex> lock(mutex_);
                for(auto& kv : histograms_) {
                    kv.second->Tick();
                }
            }
        }).detach();
    }

private:
    MetricsRegistry() {
        StartTicker();
    }
    
    std::string BuildKey(const std::string& name, const std::map<std::string, std::string>& labels);
    std::string LabelsToString(const std::map<std::string, std::string>& labels, const std::string& extra_key = "", const std::string& extra_val = "");

    std::shared_mutex mutex_;
    std::map<std::string, MetricMeta> metas_;
    std::map<std::string, std::shared_ptr<Counter>> counters_;
    std::map<std::string, std::shared_ptr<Gauge>> gauges_;
    std::map<std::string, std::shared_ptr<Histogram>> histograms_;
};

} // namespace metrics
