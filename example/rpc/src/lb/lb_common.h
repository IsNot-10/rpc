#pragma once
#include <string>

/**
 * @brief 归一化主机 Key
 * 
 * 从主机 Key 中去除权重信息，将格式从 ip:port:weight 转换为 ip:port
 * 
 * @param s 输入的主机 Key，可以是 ip:port 或 ip:port:weight 格式
 * @return std::string 归一化后的主机 Key，格式为 ip:port
 */
inline std::string normalizeHostKey(const std::string& s) {
    size_t last = s.rfind(':'); ///< 查找最后一个冒号的位置
    if (last == std::string::npos) return s; ///< 没有冒号，直接返回
    
    // 更安全的方法：查找倒数第二个冒号，判断是否为 ip:port:weight 格式
    size_t prev = (last > 0) ? s.rfind(':', last - 1) : std::string::npos;
    if (prev != std::string::npos) {
        return s.substr(0, last); ///< 存在两个冒号，返回 ip:port 部分
    }
    return s; ///< 只有一个冒号，已经是 ip:port 格式
}

#include <cstdint>
#include <random>

/**
 * @brief 负载均衡算法的通用命名空间
 * 
 * 包含负载均衡算法的通用常量和辅助函数
 */
namespace lb {

/**
 * @brief 最小延迟阈值 (微秒)
 * 
 * 对齐 BRPC 的最小延迟设置 (5ms)，用于避免极端权重并提高稳定性
 */
const int64_t MIN_LATENCY_US = 5000;

/**
 * @brief 快速线程本地随机数生成器
 * 
 * 使用 Xorshift128+ 算法生成高质量的随机数
 * 线程本地存储确保线程安全和高性能
 * 
 * @return uint64_t 生成的 64 位随机数
 */
inline uint64_t fast_rand() {
    // 线程本地随机数状态，每个线程独立初始化
    static thread_local uint64_t s[2] = { 
        (uint64_t)std::random_device{}(), ///< 使用硬件随机数初始化状态
        (uint64_t)std::random_device{}() 
    };
    
    // Xorshift128+ 算法实现
    uint64_t x = s[0];
    uint64_t const y = s[1];
    s[0] = y;
    x ^= x << 23; // 步骤 a
    s[1] = x ^ y ^ (x >> 17) ^ (y >> 26); // 步骤 b 和 c
    return s[1] + y; // 最终结果
}

/**
 * @brief 生成小于指定值的随机数
 * 
 * 生成一个范围在 [0, n) 之间的随机数
 * 
 * @param n 上限值（不包含）
 * @return uint64_t 生成的随机数
 */
inline uint64_t fast_rand_less_than(uint64_t n) {
    if (n == 0) return 0; ///< 特殊情况处理
    return fast_rand() % n; ///< 使用取模运算限制范围
}

} // namespace lb
