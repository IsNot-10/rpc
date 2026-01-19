#pragma once

#include "Connection.h"
#include <string>
#include <queue>
#include <memory>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

class ConnectionPool
{
public:
    using delFunc=std::function<void(Connection*)>;

    ~ConnectionPool();
    ConnectionPool(const ConnectionPool&)=delete;
    ConnectionPool(ConnectionPool&&)=delete;
    ConnectionPool& operator=(const ConnectionPool&)=delete;
    ConnectionPool& operator=(ConnectionPool&&)=delete;
    
    static ConnectionPool& getInstance();
    std::unique_ptr<Connection,delFunc> getConnection(); 

private:
    bool loadConfigFile();
    ConnectionPool();

private:
    std::string ip_;
    uint16_t port_;
    std::string userName_;
    std::string pwd_;
    std::string dbName_;

    //数据库连接池的初始连接数
    int initSize_;

    //数据库连接池的最大连接数
    int maxSize_;

    //每个连接的最大空闲时间,超过这个时间连接就会被踢掉
    int maxIdleTime_;

    //用户的最大等待时间,超出这个时间用户将无法获取连接
    int connectionTimeOut_;

    std::queue<Connection*> connQue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic_int connNum_;
};

