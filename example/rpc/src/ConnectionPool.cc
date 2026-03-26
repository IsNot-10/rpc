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
#include "SocketAPI.h"
#include "InetAddress.h"
#include "Logging.h"

ConnectionPool::ConnectionPool() : running_(true) {
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
            ConnectionItem item = queue.back();
            queue.pop_back();
            
            if (now - item.last_active_time > idle_timeout_sec_) {
                ::close(item.fd);
                continue;
            }
            
            char buf[1];
            int ret;
            do {
                ret = ::recv(item.fd, buf, 1, MSG_PEEK | MSG_DONTWAIT);
            } while (ret < 0 && errno == EINTR);

            if (ret == 0 || (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                ::close(item.fd);
                continue;
            }
            return item.fd;
        }
    }
    
    int clientfd = SocketAPI::createNonblocking();
    
    InetAddress server_addr(ip, port);
    
    int ret;
    do {
        ret = ::connect(clientfd, (sockaddr*)server_addr.getSockAddr(), sizeof(sockaddr_in));
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        if (errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(clientfd, &wset);
            struct timeval tv{1, 0};
            
            int s_ret;
            do {
                s_ret = select(clientfd + 1, nullptr, &wset, nullptr, &tv);
            } while (s_ret < 0 && errno == EINTR);

            if (s_ret > 0) {
                int err = SocketAPI::getSocketError(clientfd);
                if (err == 0) ret = 0; else ret = -1;
            } else {
                ret = -1;
            }
        } else {
            ret = -1;
        }
    }
    
    if (ret < 0) {
        ::close(clientfd);
        return -1;
    }
    
    int flags = fcntl(clientfd, F_GETFL, 0);
    fcntl(clientfd, F_SETFL, flags & ~O_NONBLOCK);
    
    return clientfd;
}

void ConnectionPool::releaseConnection(const std::string& ip, uint16_t port, int fd) {
    std::string key = makeKey(ip, port);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& queue = pool_[key];
    
    while (queue.size() >= max_idle_size_) {
        ::close(queue.front().fd);
        queue.pop_front();
    }
    
    queue.push_back({fd, time(nullptr)});
}

void ConnectionPool::closeConnection(int fd) {
    if (fd >= 0) ::close(fd);
}
