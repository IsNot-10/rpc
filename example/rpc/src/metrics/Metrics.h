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

/**
 * @brief 监控模块命名空间
 * 
 * 包含监控相关的组件：
 * - 计数器 (Counter)
 * - 仪表盘 (Gauge)
 * - 直方图 (Histogram)
 * - 指标注册表 (MetricsRegistry)
 * - 时间序列缓冲区 (TimeSeries)
 */
namespace metrics {

using json = nlohmann::json; ///< JSON 库别名

/**
 * @brief 计数器类
 * 
 * 单调递增的指标，用于统计事件发生的次数
 */
class Counter {
public:
    /**
     * @brief 增加计数值
     * 
     * @param v 增加的值，默认 1.0
     */
    void Inc(double v = 1.0) { 
        double current = value_.load(std::memory_order_relaxed); ///< 原子加载当前值
        // 使用 CAS 操作安全地增加计数值
        while (!value_.compare_exchange_weak(current, current + v, std::memory_order_release, std::memory_order_relaxed));
    }
    
    /**
     * @brief 获取当前计数值
     * 
     * @return double 当前计数值
     */
    double Value() const { return value_.load(std::memory_order_acquire); }
    
    /**
     * @brief 获取序列数据
     * 
     * 计数器没有序列数据，返回空数组
     * 
     * @return std::string 空的 JSON 数组字符串
     */
    std::string GetSeriesJson() const { return "[]"; }
    
private:
    std::atomic<double> value_{0.0}; ///< 原子计数值
};

/**
 * @brief 仪表盘类
 * 
 * 可增可减的指标，用于统计当前状态
 */
class Gauge {
public:
    /**
     * @brief 设置仪表盘值
     * 
     * @param v 新的仪表盘值
     */
    void Set(double v) { 
        double current = value_.load(std::memory_order_relaxed); ///< 原子加载当前值
        // 使用 CAS 操作安全地设置仪表盘值
        while (!value_.compare_exchange_weak(current, v, std::memory_order_release, std::memory_order_relaxed));
    }
    
    /**
     * @brief 增加仪表盘值
     * 
     * @param v 增加的值，默认 1.0
     */
    void Inc(double v = 1.0) { 
        double current = value_.load(std::memory_order_relaxed); ///< 原子加载当前值
        // 使用 CAS 操作安全地增加仪表盘值
        while (!value_.compare_exchange_weak(current, current + v, std::memory_order_release, std::memory_order_relaxed));
    }
    
    /**
     * @brief 减少仪表盘值
     * 
     * @param v 减少的值，默认 1.0
     */
    void Dec(double v = 1.0) { Inc(-v); } ///< 调用 Inc 方法，传入负值
    
    /**
     * @brief 获取当前仪表盘值
     * 
     * @return double 当前仪表盘值
     */
    double Value() const { return value_.load(std::memory_order_acquire); }
    
    /**
     * @brief 获取序列数据
     * 
     * 仪表盘没有序列数据，返回空数组
     * 
     * @return std::string 空的 JSON 数组字符串
     */
    std::string GetSeriesJson() const { return "[]"; }
    
private:
    std::atomic<double> value_{0.0}; ///< 原子仪表盘值
};

/**
 * @brief 线程安全的时间序列缓冲区
 * 
 * 用于存储时间序列数据，保留最近 60 个点
 */
struct TimeSeries {
    std::deque<std::pair<int64_t, double>> data; ///< 时间序列数据，格式为 (时间戳, 值)
    mutable std::mutex mutex; ///< 保护数据的互斥锁

    /**
     * @brief 添加时间序列数据点
     * 
     * @param ts 时间戳（毫秒）
     * @param val 数值
     */
    void Add(int64_t ts, double val) {
        std::lock_guard<std::mutex> lock(mutex); ///< 加锁保护临界区
        data.push_back({ts, val}); ///< 添加数据点
        // 保留最近 60 个点（假设每秒一次，即 1 分钟）
        if (data.size() > 60) {
            data.pop_front(); ///< 移除最旧的数据点
        }
    }

    /**
     * @brief 转换为 JSON 格式
     * 
     * @return json 时间序列数据的 JSON 对象
     */
    json ToJson() const {
        std::lock_guard<std::mutex> lock(mutex); ///< 加锁保护临界区
        json j = json::array(); ///< 创建 JSON 数组
        for (const auto& p : data) {
            j.push_back({p.first, p.second}); ///< 添加数据点到 JSON 数组
        }
        return j;
    }
};

/**
 * @brief 直方图类
 * 
 * 用于统计延迟分布、QPS 等指标
 */
class Histogram {
public:
    /**
     * @brief 构造函数
     */
    Histogram() {
        // 初始化上次 tick 时间为当前时间（毫秒）
        last_tick_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /**
     * @brief 记录观测值
     * 
     * @param val 观测值（秒）
     */
    void Observe(double val) {
        recorder_.Record((int64_t)(val * 1000000)); ///< 转换为微秒后记录
    }
    
    /**
     * @brief 获取每个桶的计数
     * 
     * @return std::vector<int64_t> 每个桶的计数
     */
    std::vector<int64_t> GetCounts() { return recorder_.GetCounts(); }
    
    /**
     * @brief 获取桶边界
     * 
     * @return const std::vector<int64_t>& 桶边界数组
     */
    const std::vector<int64_t>& GetBuckets() const { return recorder_.GetBuckets(); }
    
    /**
     * @brief 获取观测值总和
     * 
     * @return double 观测值总和（近似值）
     */
    double GetSum() { return recorder_.GetAvgLatency() * recorder_.GetCount(); } // 近似值
    
    /**
     * @brief 获取观测值数量
     * 
     * @return int64_t 观测值数量
     */
    int64_t GetCount() { return recorder_.GetCount(); }

    /**
     * @brief 获取 QPS
     * 
     * @return double 当前 QPS
     */
    double GetQPS() { 
        std::lock_guard<std::mutex> lock(mutex_); ///< 加锁保护临界区
        return last_qps_; 
    }
    
    /**
     * @brief 获取最大延迟
     * 
     * @return int64_t 最大延迟（微秒）
     */
    int64_t GetMaxLatency() { return recorder_.GetMaxLatency(); }
    
    /**
     * @brief 获取平均延迟
     * 
     * @return double 平均延迟（微秒）
     */
    double GetAvgLatency() { return recorder_.GetAvgLatency(); }

    /**
     * @brief 获取序列数据
     * 
     * @return std::string JSON 格式的序列数据
     * 格式: [{"label": "QPS", "data": [[ts, val], ...]}, ...]
     */
    std::string GetSeriesJson() {
        json j = json::array();
        
        // 添加 QPS 序列数据
        {
            json item;
            item["label"] = "QPS";
            item["data"] = qps_series_.ToJson();
            j.push_back(item);
        }
        
        // 添加最大延迟序列数据
        {
            json item;
            item["label"] = "Max Latency";
            item["data"] = max_lat_series_.ToJson();
            item["yaxis"] = 2; ///< 使用第二个 Y 轴
            j.push_back(item);
        }
        
        // 添加平均延迟序列数据
        {
            json item;
            item["label"] = "Avg Latency";
            item["data"] = avg_lat_series_.ToJson();
            item["yaxis"] = 2; ///< 使用第二个 Y 轴
            j.push_back(item);
        }

        return j.dump(); ///< 转换为 JSON 字符串
    }
    
    /**
     * @brief 定时更新 QPS 和历史数据
     * 
     * 每秒调用一次，更新 QPS 并记录历史数据
     */
    void Tick() {
        // 获取当前时间（纳秒精度）
        auto now_steady = std::chrono::steady_clock::now();
        // 转换为毫秒
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_steady.time_since_epoch()).count();
        
        // 获取系统时间（用于时间戳）
        auto now_system = std::chrono::system_clock::now();
        // 转换为毫秒时间戳
        int64_t now_ts = std::chrono::duration_cast<std::chrono::milliseconds>(now_system.time_since_epoch()).count();
            
        // 获取当前计数和延迟数据
        int64_t current_count = recorder_.GetCount();
        double current_max_lat = (double)recorder_.GetMaxLatency() / 1000000.0; // 转换为秒
        double current_avg_lat = recorder_.GetAvgLatency() / 1000000.0; // 转换为秒

        std::lock_guard<std::mutex> lock(mutex_); ///< 加锁保护临界区
        
        // 计算 QPS
        if (last_tick_time_ > 0) {
            double delta_sec = (now_ms - last_tick_time_) / 1000.0; ///< 计算时间差（秒）
            if (delta_sec > 0) {
                last_qps_ = (current_count - last_count_) / delta_sec; ///< 计算 QPS
                if (last_qps_ < 0) last_qps_ = 0; ///< 确保 QPS 非负
            }
        }
        
        // 记录历史数据
        qps_series_.Add(now_ts, last_qps_); ///< 添加 QPS 数据点
        max_lat_series_.Add(now_ts, current_max_lat); ///< 添加最大延迟数据点
        avg_lat_series_.Add(now_ts, current_avg_lat); ///< 添加平均延迟数据点

        // 更新上次计数和时间
        last_count_ = current_count;
        last_tick_time_ = now_ms;
    }

private:
    LatencyRecorder recorder_; ///< 延迟记录器
    std::mutex mutex_; ///< 保护临界区的互斥锁
    int64_t last_tick_time_ = 0; ///< 上次 tick 时间（毫秒）
    int64_t last_count_ = 0; ///< 上次计数
    double last_qps_ = 0.0; ///< 上次 QPS

    TimeSeries qps_series_; ///< QPS 时间序列
    TimeSeries max_lat_series_; ///< 最大延迟时间序列
    TimeSeries avg_lat_series_; ///< 平均延迟时间序列
};

/**
 * @brief 指标元数据结构体
 * 
 * 存储指标的元信息
 */
struct MetricMeta {
    std::string name; ///< 指标名称
    std::string help; ///< 指标帮助信息
    std::map<std::string, std::string> labels; ///< 指标标签
    std::string type; ///< 指标类型
};

/**
 * @brief 指标注册表类
 * 
 * 管理所有指标，提供指标的创建、查询和导出功能
 */
class MetricsRegistry {
public:
    /**
     * @brief 获取单例实例
     * 
     * @return MetricsRegistry& 单例实例的引用
     */
    static MetricsRegistry& instance();

    /**
     * @brief 获取或创建计数器
     * 
     * @param name 指标名称
     * @param help 指标帮助信息
     * @param labels 指标标签
     * @return std::shared_ptr<Counter> 计数器智能指针
     */
    std::shared_ptr<Counter> GetCounter(const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels);
    
    /**
     * @brief 获取或创建仪表盘
     * 
     * @param name 指标名称
     * @param help 指标帮助信息
     * @param labels 指标标签
     * @return std::shared_ptr<Gauge> 仪表盘智能指针
     */
    std::shared_ptr<Gauge> GetGauge(const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels);
    
    /**
     * @brief 获取或创建直方图
     * 
     * @param name 指标名称
     * @param help 指标帮助信息
     * @param labels 指标标签
     * @return std::shared_ptr<Histogram> 直方图智能指针
     */
    std::shared_ptr<Histogram> GetHistogram(const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels);

    /**
     * @brief 转换为 Prometheus 格式
     * 
     * @return std::string Prometheus 格式的指标字符串
     */
    std::string ToPrometheus();
    
    /**
     * @brief 返回用于前端展示的 JSON
     * 
     * @return std::string JSON 格式的指标数据
     */
    std::string ToJson() {
        std::shared_lock<std::shared_mutex> lock(mutex_); ///< 共享锁保护临界区
        json j;
        j["metrics"] = json::array(); ///< 创建 metrics 数组

        // 添加所有计数器
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

        // 添加所有仪表盘
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

        // 添加所有直方图
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
        
        return j.dump(); ///< 转换为 JSON 字符串
    }

    /**
     * @brief 获取指定名称的序列数据
     * 
     * @param name 指标名称
     * @return std::string 序列数据的 JSON 字符串
     */
    std::string GetSeriesJson(const std::string& name) {
        std::shared_lock<std::shared_mutex> lock(mutex_); ///< 共享锁保护临界区
        if (histograms_.find(name) != histograms_.end()) {
            return histograms_[name]->GetSeriesJson();
        }
        return "[]"; ///< 未找到指标，返回空数组
    }

    /**
     * @brief 启动 ticker 线程
     * 
     * 每秒调用一次所有直方图的 Tick 方法
     */
    void StartTicker() {
        std::thread([this](){
            while(true) {
                std::this_thread::sleep_for(std::chrono::seconds(1)); ///< 每秒执行一次
                std::shared_lock<std::shared_mutex> lock(mutex_); ///< 共享锁保护临界区
                for(auto& kv : histograms_) {
                    kv.second->Tick(); ///< 调用直方图的 Tick 方法
                }
            }
        }).detach(); ///< 分离线程
    }

private:
    /**
     * @brief 构造函数
     */
    MetricsRegistry() {
        StartTicker(); ///< 启动 ticker 线程
    }
    
    /**
     * @brief 构建指标键
     * 
     * @param name 指标名称
     * @param labels 指标标签
     * @return std::string 构建的指标键
     */
    std::string BuildKey(const std::string& name, const std::map<std::string, std::string>& labels);
    
    /**
     * @brief 将标签转换为字符串
     * 
     * @param labels 指标标签
     * @param extra_key 额外的键
     * @param extra_val 额外的值
     * @return std::string 标签字符串
     */
    std::string LabelsToString(const std::map<std::string, std::string>& labels, const std::string& extra_key = "", const std::string& extra_val = "");

    std::shared_mutex mutex_; ///< 保护指标映射的共享互斥锁
    std::map<std::string, MetricMeta> metas_; ///< 指标元数据映射
    std::map<std::string, std::shared_ptr<Counter>> counters_; ///< 计数器映射
    std::map<std::string, std::shared_ptr<Gauge>> gauges_; ///< 仪表盘映射
    std::map<std::string, std::shared_ptr<Histogram>> histograms_; ///< 直方图映射
};

} // namespace metrics
