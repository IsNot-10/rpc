#include "ConnectionPool.h"
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#include <iostream>
#include <thread>
#include <chrono>

ConnectionPool::ConnectionPool() : running_(true) {
    // 可以在此处从配置文件加载参数
    cleaner_thread_ = std::thread([this]() { this->cleanerLoop(); });
}

ConnectionPool::~ConnectionPool() {
    running_ = false;
    if (cleaner_thread_.joinable()) {
        cleaner_thread_.join();
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : pool_) {
        for (auto& item : pair.second) {
            ::close(item.fd);
        }
    }
    pool_.clear();
}

void ConnectionPool::cleanerLoop() {
    while (running_) {
        // 每 5 秒检查一次
        for (int i = 0; i < 50; ++i) {
            if (!running_) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::lock_guard<std::mutex> lock(mutex_);
        time_t now = time(nullptr);
        
        for (auto it = pool_.begin(); it != pool_.end(); ) {
            auto& queue = it->second;
            for (auto q_it = queue.begin(); q_it != queue.end(); ) {
                if (now - q_it->last_active_time > idle_timeout_sec_) {
                    ::close(q_it->fd);
                    q_it = queue.erase(q_it);
                } else {
                    ++q_it;
                }
            }
            
            if (queue.empty()) {
                it = pool_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

ConnectionPool& ConnectionPool::instance() {
    static ConnectionPool pool;
    return pool;
}

std::string ConnectionPool::makeKey(const std::string& ip, uint16_t port) {
    return ip + ":" + std::to_string(port);
}

int ConnectionPool::getConnection(const std::string& ip, uint16_t port) {
    std::string key = makeKey(ip, port);
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& queue = pool_[key];
        time_t now = time(nullptr);
        
        while (!queue.empty()) {
            // LIFO: Get the most recently used connection (better for cache locality)
            ConnectionItem item = queue.back();
            queue.pop_back();
            
            // 检查空闲超时
            if (now - item.last_active_time > idle_timeout_sec_) {
                ::close(item.fd);
                continue;
            }
            
            // 使用 MSG_PEEK 检查连接是否仍然存活
            // 只要没有 RST 或 EOF，就认为连接可用
            char buf[1];
            int ret = ::recv(item.fd, buf, 1, MSG_PEEK | MSG_DONTWAIT);
            if (ret == 0 || (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                ::close(item.fd);
                continue;
            }
            return item.fd;
        }
    }
    
    // 创建新连接
    int clientfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (clientfd == -1) return -1;
    
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr);
    
    // 带超时连接 (1秒)
    int flags = fcntl(clientfd, F_GETFL, 0);
    fcntl(clientfd, F_SETFL, flags | O_NONBLOCK);
    
    int ret = ::connect(clientfd, (sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        if (errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(clientfd, &wset);
            struct timeval tv{1, 0}; 
            ret = select(clientfd + 1, nullptr, &wset, nullptr, &tv);
            if (ret > 0) {
                int so_error;
                socklen_t len = sizeof(so_error);
                getsockopt(clientfd, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (so_error == 0) ret = 0; else ret = -1;
            } else {
                ret = -1; // 超时或错误
            }
        } else {
            ret = -1;
        }
    }
    
    if (ret < 0) {
        ::close(clientfd);
        return -1;
    }
    
    // 恢复阻塞模式 (MpRpcChannel 期望使用带超时的阻塞读)
    fcntl(clientfd, F_SETFL, flags);
    
    return clientfd;
}

void ConnectionPool::releaseConnection(const std::string& ip, uint16_t port, int fd) {
    std::string key = makeKey(ip, port);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& queue = pool_[key];
    
    // 如果池满了，移除最旧的连接 (front)，腾出空间给最新的 (back)
    while (queue.size() >= max_idle_size_) {
        ::close(queue.front().fd);
        queue.pop_front();
    }
    
    queue.push_back({fd, time(nullptr)});
}

void ConnectionPool::closeConnection(int fd) {
    if (fd >= 0) ::close(fd);
}
