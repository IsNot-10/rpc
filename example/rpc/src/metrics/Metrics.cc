#include "Metrics.h"
#include <sstream>
#include <iomanip>

namespace metrics {

MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry instance;
    return instance;
}

std::string MetricsRegistry::BuildKey(const std::string& name, const std::map<std::string, std::string>& labels) {
    std::stringstream ss;
    ss << name;
    for (const auto& kv : labels) {
        ss << "|" << kv.first << "=" << kv.second;
    }
    return ss.str();
}

std::string MetricsRegistry::LabelsToString(const std::map<std::string, std::string>& labels, const std::string& extra_key, const std::string& extra_val) {
    if (labels.empty() && extra_key.empty()) return "";
    std::stringstream ss;
    ss << "{";
    bool first = true;
    for (const auto& kv : labels) {
        if (!first) ss << ",";
        ss << kv.first << "=\"" << kv.second << "\"";
        first = false;
    }
    if (!extra_key.empty()) {
        if (!first) ss << ",";
        ss << extra_key << "=\"" << extra_val << "\"";
    }
    ss << "}";
    return ss.str();
}

std::shared_ptr<Counter> MetricsRegistry::GetCounter(const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels) {
    std::string key = BuildKey(name, labels);
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = counters_.find(key);
        if (it != counters_.end()) return it->second;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (counters_.find(key) == counters_.end()) {
        counters_[key] = std::make_shared<Counter>();
        metas_[key] = {name, help, labels, "counter"};
    }
    return counters_[key];
}

std::shared_ptr<Gauge> MetricsRegistry::GetGauge(const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels) {
    std::string key = BuildKey(name, labels);
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = gauges_.find(key);
        if (it != gauges_.end()) return it->second;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (gauges_.find(key) == gauges_.end()) {
        gauges_[key] = std::make_shared<Gauge>();
        metas_[key] = {name, help, labels, "gauge"};
    }
    return gauges_[key];
}

std::shared_ptr<Histogram> MetricsRegistry::GetHistogram(const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels) {
    std::string key = BuildKey(name, labels);
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = histograms_.find(key);
        if (it != histograms_.end()) return it->second;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (histograms_.find(key) == histograms_.end()) {
        histograms_[key] = std::make_shared<Histogram>();
        metas_[key] = {name, help, labels, "histogram"};
    }
    return histograms_[key];
}

std::string MetricsRegistry::ToPrometheus() {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::stringstream ss;
    
    // 辅助函数：按名称对指标进行分组（目前简单迭代）
    // 理想情况下，每个名称只打印一次 HELP 和 TYPE。
    
    std::map<std::string, bool> header_printed;
    
    // 处理计数器 (Counters)
    for (const auto& kv : counters_) {
        const auto& meta = metas_[kv.first];
        if (!header_printed[meta.name]) {
            ss << "# HELP " << meta.name << " " << meta.help << "\n";
            ss << "# TYPE " << meta.name << " counter\n";
            header_printed[meta.name] = true;
        }
        ss << meta.name << LabelsToString(meta.labels) << " " << kv.second->Value() << "\n";
    }
    
    // 处理仪表盘 (Gauges)
    for (const auto& kv : gauges_) {
        const auto& meta = metas_[kv.first];
        if (!header_printed[meta.name]) {
            ss << "# HELP " << meta.name << " " << meta.help << "\n";
            ss << "# TYPE " << meta.name << " gauge\n";
            header_printed[meta.name] = true;
        }
        ss << meta.name << LabelsToString(meta.labels) << " " << kv.second->Value() << "\n";
    }
    
    // 处理直方图 (Histograms)
    for (const auto& kv : histograms_) {
        const auto& meta = metas_[kv.first];
        if (!header_printed[meta.name]) {
            ss << "# HELP " << meta.name << " " << meta.help << "\n";
            ss << "# TYPE " << meta.name << " histogram\n";
            header_printed[meta.name] = true;
        }
        
        auto counts = kv.second->GetCounts();
        const auto& buckets = kv.second->GetBuckets();
        int64_t cumulative_count = 0;
        
        // 桶 (Buckets)
        for (size_t i = 0; i < buckets.size(); ++i) {
            cumulative_count += counts[i];
            // 将微秒转换为秒字符串
            std::string le = std::to_string(buckets[i] / 1000000.0); 
            // 移除尾随的零
            le.erase ( le.find_last_not_of('0') + 1, std::string::npos );
            if(le.back() == '.') le.pop_back();

            ss << meta.name << "_bucket" << LabelsToString(meta.labels, "le", le) << " " << cumulative_count << "\n";
        }
        
        // +Inf 桶
        cumulative_count += counts.back();
        ss << meta.name << "_bucket" << LabelsToString(meta.labels, "le", "+Inf") << " " << cumulative_count << "\n";
        
        // 总和 (Sum) 和 计数 (Count)
        ss << meta.name << "_sum" << LabelsToString(meta.labels) << " " << kv.second->GetSum() << "\n";
        ss << meta.name << "_count" << LabelsToString(meta.labels) << " " << cumulative_count << "\n";
        
        // 窗口统计 (QPS 和 最大延迟)
        ss << meta.name << "_qps" << LabelsToString(meta.labels) << " " << kv.second->GetQPS() << "\n";
        ss << meta.name << "_max_latency_seconds" << LabelsToString(meta.labels) << " " << (kv.second->GetMaxLatency() / 1000000.0) << "\n";
    }
    
    return ss.str();
}

} // namespace metrics
