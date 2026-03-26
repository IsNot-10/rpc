#pragma once
#include <string>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <ctime>
#include <thread>
#include <atomic>

/**
 * @brief 连接池中的连接项结构体
 * 
 * 存储单个连接的文件描述符和最后活跃时间
 */
struct ConnectionItem {
    int fd;                 ///< 连接的文件描述符
    time_t last_active_time; ///< 最后活跃时间 (用于空闲超时检查)
};

/**
 * @brief 连接池类
 * 
 * 管理TCP连接的复用，减少连接建立和关闭的开销
 * 支持空闲连接超时回收和最大空闲连接数限制
 */
class ConnectionPool {
public:
    /**
     * @brief 获取连接池的单例实例
     * 
     * @return ConnectionPool& 连接池实例的引用
     */
    static ConnectionPool& instance();
    
    /**
     * @brief 获取一个连接
     * 
     * 如果池中有空闲连接则复用，否则新建连接
     * 
     * @param ip 服务器IP地址
     * @param port 服务器端口号
     * @return int 连接的文件描述符，返回 -1 表示失败
     */
    int getConnection(const std::string& ip, uint16_t port);
    
    /**
     * @brief 释放连接回连接池
     * 
     * 将连接放回池中，以便复用
     * 
     * @param ip 服务器IP地址
     * @param port 服务器端口号
     * @param fd 连接的文件描述符
     */
    void releaseConnection(const std::string& ip, uint16_t port, int fd);
    
    /**
     * @brief 关闭损坏的连接
     * 
     * 关闭连接但不放回池中
     * 
     * @param fd 连接的文件描述符
     */
    void closeConnection(int fd);

    /**
     * @brief 设置每个主机的最大空闲连接数
     * 
     * @param n 最大空闲连接数
     */
    void setMaxIdleConnections(size_t n) { max_idle_size_ = n; }
    
    /**
     * @brief 设置空闲连接超时时间
     * 
     * @param seconds 超时时间（秒）
     */
    void setIdleTimeout(int seconds) { idle_timeout_sec_ = seconds; }

private:
    /**
     * @brief 连接池的构造函数
     */
    ConnectionPool();
    
    /**
     * @brief 连接池的析构函数
     */
    ~ConnectionPool();
    
    /**
     * @brief 生成连接池的键
     * 
     * @param ip 服务器IP地址
     * @param port 服务器端口号
     * @return std::string 生成的键，格式为 "ip:port"
     */
    std::string makeKey(const std::string& ip, uint16_t port);
    
    /**
     * @brief 清理线程的主循环
     * 
     * 定期检查并清理超时的空闲连接
     */
    void cleanerLoop();
    
    std::mutex mutex_; ///< 保护连接池的互斥锁
    
    /**
     * @brief 连接池的核心数据结构
     * 
     * 键为 "ip:port"，值为该主机的空闲连接队列
     */
    std::unordered_map<std::string, std::deque<ConnectionItem>> pool_;
    
    size_t max_idle_size_ = 50; ///< 每个 Host 的最大空闲连接数
    int idle_timeout_sec_ = 60; ///< 空闲超时时间 (秒)
    
    std::atomic<bool> running_; ///< 清理线程运行标志
    std::thread cleaner_thread_; ///< 定期清理超时连接的线程
};
