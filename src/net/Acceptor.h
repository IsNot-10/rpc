#pragma once

#include "Socket.h"
#include "Channel.h"

class EventLoop;


//Acceptor对象封装的正是listenfd,可以说相比Socket类它才是真正的RAII类
//它负责listenfd的创建,绑定地址,监听和接收连接
class Acceptor
:NonCopy
{
public:
    using NewConnectionCallback=std::function<void(int,const InetAddress&)>;

public:
    Acceptor(EventLoop* loop,const InetAddress& localAddr,bool reuseport);
    ~Acceptor();
    void listen();

    void setNewConnectionCallback(NewConnectionCallback cb)
    {
        newConnectionCallback_=std::move(cb);
    }

    bool listenning()const
    {
        return listenning_;
    }

private:
    //处理listenfd的读回调操作,很明显是调用accept函数(muduo中是accept4)
    void handleRead(TimeStamp receiveTime);

private:
    //listenfd当然是在主线程中的
    //那么这里的loop_当然也对应的是mainLoop了
    EventLoop* loop_;
    Socket acceptSocket_;

    //负责listenfd和它关注的事件,listenfd只关注读事件不关注写事件
    Channel acceptChannel_;
    bool listenning_;

    //连接回调函数,由TcpServer对象给当前Acceptor对象设置
    NewConnectionCallback newConnectionCallback_;

    //防止连接fd数量超过上限,用于占位的fd
    int idleFd_;
};

