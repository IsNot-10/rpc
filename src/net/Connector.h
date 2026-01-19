#pragma once

#include "NonCopy.h"
#include "InetAddress.h"
#include "TimerId.h"
#include <functional>
#include <memory>

class Channel;
class EventLoop;



//客户端
class Connector
:NonCopy
{
private:

    //只有调用::connect函数成功返回成功才会从kDisConnected变为kConnecting
    //这时候客户端的sockfd是try_connfd,Channel对象才会开始起作用并关注写事件
    //实际上Channel已经可写了,但是不意味着客户端和服务端真的连接成功了
    
    //只有客户端::connect函数返回成功的基础上没有错误和自连接问题,状态才会从
    //kConnecting变为kConnected,这时候客户端的sockfd是connfd
    //即将处理tcp连接事件了
    enum class StateE {kDisconnected,kConnecting,kConnected};
    
    
    static constexpr int kMaxRetryDelayMs=30*1000;
    static constexpr int kInitRetryDelayMs=500;

public:
    using NewConnectionCallback=std::function<void(int)>;

    Connector(EventLoop* loop,const InetAddress& serverAddr);
    ~Connector();
    void start();
    void stop();
    void restart();

    const InetAddress& getServerAddr()const
    {
        return serverAddr_;
    }

    void setNewConnectionCallback(NewConnectionCallback cb)
    {
        newConnectionCallback_=std::move(cb);
    }

private:
    void startInLoop();
    void stopInLoop();
    void connect();
    void connecting(int sockfd);
    void retry(int sockfd);
    int removeAndResetChannel();
    void resetChannel();
    void handleWrite();
    void handleError();
    void cancelRetryTimer();

    void setState(StateE state)
    {
        state_=state;
    }

private:
    EventLoop* loop_;
    InetAddress serverAddr_;
    bool connect_;
    StateE state_;

    //封装try_connfd的Channel对象
    //客户端创建的socketfd,在客户端和服务端连接真正成功以前的形态我称为try_connfd
    std::unique_ptr<Channel> channel_;

    //如果客户端::connect返回成功也没有错误和自连接,肯定会生成TcpConnection对象的
    //这个回调函数是由TcpClient对象提供的
    NewConnectionCallback newConnectionCallback_;

    //connect失败重试时间(一般每次都会倍增)
    int retryDelayMs_;

    //当前加入的定时器(会在retryDelayMs_时间之后调用超时回调函数)
    //超时回调就是 重试(新创建sockfd再::connect)
    TimerId retryTimerId_;
};

