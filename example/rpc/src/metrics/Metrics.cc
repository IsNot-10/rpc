#include "Metrics.h"
#include <sstream>
#include <iomanip>

namespace metrics {

/**
 * @brief 获取指标注册表的单例实例
 * 
 * @return MetricsRegistry& 指标注册表实例的引用
 */
MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry instance; ///< 静态局部变量，实现线程安全的单例模式
    return instance;
}

/**
 * @brief 构建指标的唯一键
 * 
 * 将指标名称和标签组合成唯一键，用于在映射中存储和查找指标
 * 
 * @param name 指标名称
 * @param labels 指标标签映射
 * @return std::string 构建的唯一键
 */
std::string MetricsRegistry::BuildKey(const std::string& name, const std::map<std::string, std::string>& labels) {
    std::stringstream ss; ///< 字符串流，用于构建键
    ss << name; ///< 添加指标名称
    for (const auto& kv : labels) { ///< 遍历所有标签
        ss << "|" << kv.first << "=" << kv.second; ///< 添加标签，格式：|key=value
    }
    return ss.str(); ///< 返回构建的键
}

/**
 * @brief 将标签转换为字符串
 * 
 * 将标签映射转换为 Prometheus 格式的标签字符串
 * 
 * @param labels 指标标签映射
 * @param extra_key 额外的键（可选）
 * @param extra_val 额外的值（可选）
 * @return std::string 转换后的标签字符串
 */
std::string MetricsRegistry::LabelsToString(const std::map<std::string, std::string>& labels, const std::string& extra_key, const std::string& extra_val) {
    if (labels.empty() && extra_key.empty()) return ""; ///< 没有标签，返回空字符串
    std::stringstream ss; ///< 字符串流，用于构建标签字符串
    ss << "{"; ///< 标签字符串开始
    bool first = true; ///< 标记是否是第一个标签
    for (const auto& kv : labels) { ///< 遍历所有标签
        if (!first) ss << ","; ///< 添加标签分隔符
        ss << kv.first << "=\"" << kv.second << "\""; ///< 添加标签，格式：key="value"
        first = false; ///< 更新第一个标签标记
    }
    if (!extra_key.empty()) { ///< 处理额外的标签
        if (!first) ss << ","; ///< 添加标签分隔符
        ss << extra_key << "=\"" << extra_val << "\""; ///< 添加额外标签
    }
    ss << "}"; ///< 标签字符串结束
    return ss.str(); ///< 返回构建的标签字符串
}

/**
 * @brief 获取或创建计数器
 * 
 * @param name 指标名称
 * @param help 指标帮助信息
 * @param labels 指标标签映射
 * @return std::shared_ptr<Counter> 计数器的智能指针
 */
std::shared_ptr<Counter> MetricsRegistry::GetCounter(const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels) {
    std::string key = BuildKey(name, labels); ///< 构建指标的唯一键
    {
        std::shared_lock<std::shared_mutex> lock(mutex_); ///< 共享锁
        auto it = counters_.find(key); ///< 查找计数器
        if (it != counters_.end()) return it->second; ///< 找到计数器，返回
    }
    std::unique_lock<std::shared_mutex> lock(mutex_); ///< 独占锁
    // 双重检查，防止在获取写锁期间其他线程已创建计数器
    if (counters_.find(key) == counters_.end()) {
        counters_[key] = std::make_shared<Counter>(); ///< 创建新计数器
        metas_[key] = {name, help, labels, "counter"}; ///< 存储指标元数据
    }
    return counters_[key]; ///< 返回计数器
}

/**
 * @brief 获取或创建仪表盘
 * 
 * @param name 指标名称
 * @param help 指标帮助信息
 * @param labels 指标标签映射
 * @return std::shared_ptr<Gauge> 仪表盘的智能指针
 */
std::shared_ptr<Gauge> MetricsRegistry::GetGauge(const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels) {
    std::string key = BuildKey(name, labels); ///< 构建指标的唯一键
    {
        std::shared_lock<std::shared_mutex> lock(mutex_); ///< 共享锁
        auto it = gauges_.find(key); ///< 查找仪表盘
        if (it != gauges_.end()) return it->second; ///< 找到仪表盘，返回
    }
    std::unique_lock<std::shared_mutex> lock(mutex_); ///< 独占锁
    // 双重检查，防止在获取写锁期间其他线程已创建仪表盘
    if (gauges_.find(key) == gauges_.end()) {
        gauges_[key] = std::make_shared<Gauge>(); ///< 创建新仪表盘
        metas_[key] = {name, help, labels, "gauge"}; ///< 存储指标元数据
    }
    return gauges_[key]; ///< 返回仪表盘
}

/**
 * @brief 获取或创建直方图
 * 
 * @param name 指标名称
 * @param help 指标帮助信息
 * @param labels 指标标签映射
 * @return std::shared_ptr<Histogram> 直方图的智能指针
 */
std::shared_ptr<Histogram> MetricsRegistry::GetHistogram(const std::string& name, const std::string& help, const std::map<std::string, std::string>& labels) {
    std::string key = BuildKey(name, labels); ///< 构建指标的唯一键
    {
        std::shared_lock<std::shared_mutex> lock(mutex_); ///< 共享锁
        auto it = histograms_.find(key); ///< 查找直方图
        if (it != histograms_.end()) return it->second; ///< 找到直方图，返回
    }
    std::unique_lock<std::shared_mutex> lock(mutex_); ///< 独占锁
    // 双重检查，防止在获取写锁期间其他线程已创建直方图
    if (histograms_.find(key) == histograms_.end()) {
        histograms_[key] = std::make_shared<Histogram>(); ///< 创建新直方图
        metas_[key] = {name, help, labels, "histogram"}; ///< 存储指标元数据
    }
    return histograms_[key]; ///< 返回直方图
}

/**
 * @brief 将所有指标转换为 Prometheus 格式
 * 
 * 生成符合 Prometheus 监控格式的指标字符串
 * 
 * @return std::string Prometheus 格式的指标字符串
 */
std::string MetricsRegistry::ToPrometheus() {
    std::shared_lock<std::shared_mutex> lock(mutex_); ///< 共享锁
    std::stringstream ss; ///< 字符串流，用于构建 Prometheus 格式的指标
    
    std::map<std::string, bool> header_printed; ///< 记录哪些指标名称的 HELP 和 TYPE 已经打印
    
    // 处理计数器 (Counters)
    for (const auto& kv : counters_) { ///< 遍历所有计数器
        const auto& meta = metas_[kv.first]; ///< 获取指标元数据
        if (!header_printed[meta.name]) { ///< 未打印过该指标的 HELP 和 TYPE
            ss << "# HELP " << meta.name << " " << meta.help << "\n"; ///< 打印 HELP 行
            ss << "# TYPE " << meta.name << " counter\n"; ///< 打印 TYPE 行
            header_printed[meta.name] = true; ///< 标记为已打印
        }
        ss << meta.name << LabelsToString(meta.labels) << " " << kv.second->Value() << "\n"; ///< 打印指标值
    }
    
    // 处理仪表盘 (Gauges)
    for (const auto& kv : gauges_) { ///< 遍历所有仪表盘
        const auto& meta = metas_[kv.first]; ///< 获取指标元数据
        if (!header_printed[meta.name]) { ///< 未打印过该指标的 HELP 和 TYPE
            ss << "# HELP " << meta.name << " " << meta.help << "\n"; ///< 打印 HELP 行
            ss << "# TYPE " << meta.name << " gauge\n"; ///< 打印 TYPE 行
            header_printed[meta.name] = true; ///< 标记为已打印
        }
        ss << meta.name << LabelsToString(meta.labels) << " " << kv.second->Value() << "\n"; ///< 打印指标值
    }
    
    // 处理直方图 (Histograms)
    for (const auto& kv : histograms_) { ///< 遍历所有直方图
        const auto& meta = metas_[kv.first]; ///< 获取指标元数据
        if (!header_printed[meta.name]) { ///< 未打印过该指标的 HELP 和 TYPE
            ss << "# HELP " << meta.name << " " << meta.help << "\n"; ///< 打印 HELP 行
            ss << "# TYPE " << meta.name << " histogram\n"; ///< 打印 TYPE 行
            header_printed[meta.name] = true; ///< 标记为已打印
        }
        
        auto counts = kv.second->GetCounts(); ///< 获取每个桶的计数
        const auto& buckets = kv.second->GetBuckets(); ///< 获取桶边界
        int64_t cumulative_count = 0; ///< 累积计数
        
        // 打印各个桶的累积计数
        for (size_t i = 0; i < buckets.size(); ++i) {
            cumulative_count += counts[i]; ///< 累积计数
            // 将微秒转换为秒字符串
            std::string le = std::to_string(buckets[i] / 1000000.0); 
            // 移除尾随的零，使输出更简洁
            le.erase ( le.find_last_not_of('0') + 1, std::string::npos );
            if(le.back() == '.') le.pop_back(); ///< 移除末尾的点

            ss << meta.name << "_bucket" << LabelsToString(meta.labels, "le", le) << " " << cumulative_count << "\n"; ///< 打印桶的累积计数
        }
        
        // 打印 +Inf 桶的累积计数
        cumulative_count += counts.back(); ///< 加上最后一个桶的计数
        ss << meta.name << "_bucket" << LabelsToString(meta.labels, "le", "+Inf") << " " << cumulative_count << "\n"; ///< 打印 +Inf 桶
        
        // 打印总和 (Sum) 和 计数 (Count)
        ss << meta.name << "_sum" << LabelsToString(meta.labels) << " " << kv.second->GetSum() << "\n"; ///< 打印总和
        ss << meta.name << "_count" << LabelsToString(meta.labels) << " " << cumulative_count << "\n"; ///< 打印计数
        
        // 打印窗口统计 (QPS 和 最大延迟)
        ss << meta.name << "_qps" << LabelsToString(meta.labels) << " " << kv.second->GetQPS() << "\n"; ///< 打印 QPS
        ss << meta.name << "_max_latency_seconds" << LabelsToString(meta.labels) << " " << (kv.second->GetMaxLatency() / 1000000.0) << "\n"; ///< 打印最大延迟（转换为秒）
    }
    
    return ss.str(); ///< 返回 Prometheus 格式的指标字符串
}

} // namespace metrics
