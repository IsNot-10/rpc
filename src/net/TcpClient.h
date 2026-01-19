#pragma once

#include "NonCopy.h"
#include "Callbacks.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "TcpConnection.h"

class Connector;


//muduo的客户端
class TcpClient
:NonCopy
{
public:
    TcpClient(EventLoop* loop,const InetAddress& serverAddr
        ,std::string_view name);
    ~TcpClient();
    void connect();
    void disconnect();
    void stop();

    //这三个回调都由用户设置
    void setNewConnectionCallback(ConnectionCallback cb)
    {
        connectionCallback_=std::move(cb);
    }

    void setMessageCallback(MessageCallback cb)
    {
        messageCallback_=std::move(cb);
    }

    void setWriteCompleteCallback(WriteCompleteCallback cb)
    {
        writeCompleteCallback_=std::move(cb);
    }

    EventLoop* getLoop()const
    {
        return loop_;
    }

    bool isRetry()const
    {
        return retry_;
    }

    void enableRetry()
    {
        retry_=true;
    }

    const std::string& getName()const
    {
        return name_;
    }

    TcpConnectionPtr getConnection()const
    {
        std::lock_guard<std::mutex> lock{mtx_};
        return conn_;
    }

private:
    void newConnection(int sockfd);
    void removeConnection(const TcpConnectionPtr& conn);

private:
    //客户端只有一个IO线程(mainLoop)
    EventLoop* loop_;

    //实现connect失败需要反复尝试的逻辑
    std::unique_ptr<Connector> connector_;
    
    const std::string name_;
    
    //是否支持在tcp连接断开的时候继续connect重连
    //默认不支持,但是用户可以设置
    bool retry_;
    
    bool connect_;
    
    //单纯给TcpConnection对象起名字用的
    int connId_;

    //客户端当然最多只有一条tcp连接
    TcpConnectionPtr conn_;
    mutable std::mutex mtx_;
    
    //都是由用户设置的回调
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
};

