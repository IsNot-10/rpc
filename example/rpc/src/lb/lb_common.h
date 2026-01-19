#pragma once
#include <string>

// 辅助函数：归一化主机 Key (去除权重信息)
inline std::string normalizeHostKey(const std::string& s) {
    size_t last = s.rfind(':');
    if (last == std::string::npos) return s;
    // 检查最后一个 ':' 后面的部分是否为数字 (权重)
    // 简单启发式：如果有 2 个冒号，则为 ip:port:weight
    // 如果只有 1 个冒号，则为 ip:port
    // IPv6 情况? "[::1]:8080" -> 需要更复杂的解析，这里暂不处理
    // 假设本项目使用的标准格式为：ip:port 或 ip:port:weight
    
    // 更安全的方法：查找倒数第二个冒号
    size_t prev = (last > 0) ? s.rfind(':', last - 1) : std::string::npos;
    if (prev != std::string::npos) {
        return s.substr(0, last); // 返回 ip:port
    }
    return s; // 已经是 ip:port
}

#include <cstdint>

// 负载均衡算法的通用常量
namespace lb {

// 对齐 BRPC 的最小延迟 (5ms)，避免极端权重并提高稳定性
const int64_t MIN_LATENCY_US = 5000;

} // namespace lb
