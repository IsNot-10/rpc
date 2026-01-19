#pragma once
#include <string>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <ctime>
#include <thread>
#include <atomic>

struct ConnectionItem {
    int fd;                 // 连接的文件描述符
    time_t last_active_time; // 最后活跃时间 (用于空闲超时检查)
};

class ConnectionPool {
public:
    static ConnectionPool& instance();
    
    // 获取一个连接 (fd)。如果池中有空闲连接则复用，否则新建。返回 -1 表示失败。
    int getConnection(const std::string& ip, uint16_t port);
    
    // 释放连接回连接池 (连接复用)
    void releaseConnection(const std::string& ip, uint16_t port, int fd);
    
    // 关闭损坏的连接 (不放回池中)
    void closeConnection(int fd);

    // 配置参数设置
    void setMaxIdleConnections(size_t n) { max_idle_size_ = n; }
    void setIdleTimeout(int seconds) { idle_timeout_sec_ = seconds; }

private:
    ConnectionPool();
    ~ConnectionPool();
    
    std::string makeKey(const std::string& ip, uint16_t port);
    void cleanerLoop();
    
    std::mutex mutex_;
    std::unordered_map<std::string, std::deque<ConnectionItem>> pool_;
    
    size_t max_idle_size_ = 50; // 每个 Host 的最大空闲连接数
    int idle_timeout_sec_ = 60; // 空闲超时时间 (秒)
    
    std::atomic<bool> running_;
    std::thread cleaner_thread_;
};
