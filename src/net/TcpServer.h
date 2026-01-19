#pragma once

#include "EventLoop.h"
#include "Callbacks.h"
#include "InetAddress.h"
#include "TcpConnection.h"
#include "Logging.h"
#include <unordered_map>

class EventLoopThreadPool;
class Acceptor;


//TcpServer是暴露给用户的最上层类,也说明了muduo本质就是一个tcp通信架构
class TcpServer
:NonCopy
{
public:
    using ThreadInitCallback=std::function<void(EventLoop*)>;

    enum class Option
    {
        kNoReusePort,
        kReusePort,
    };

public:
    TcpServer(EventLoop* loop,const InetAddress& listenAddr,
            std::string_view name,Option option=Option::kNoReusePort);
    ~TcpServer();
    void start();
    void setThreadNum(int threadNum);

    void setThreadInitCallback(ThreadInitCallback cb)
    {
        threadInitCallback_=std::move(cb);
    }
    
    //这三个回调函数都是用户设置,最后再传给生成的TcpConnection对象
    //让它们在适当的时刻去调用
    void setConnectionCallback(ConnectionCallback cb)
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

    const std::string& getName()const
    {
        return name_;
    }

    const std::string& getIpPort()const
    {
        return ipPort_;
    }
    
private:
    //这个函数就是传给Acceptor对象的newConnectionCallback_数据成员的
    //作为listenfd就绪可读时候会先调用::accept函数生成一个connfd
    
    //后续会把connfd封装成TcpConnection对象,这一步正是由这个函数完成
    void newConnection(int connfd,const InetAddress& peerAddr);
    
    //必定跨线程,会间接调用removeConnectionLoop
    void removeConnection(const TcpConnectionPtr& conn);
    
    //完成了"连接断开"的一部分逻辑,主要是把TcpConnection从connMap_中删除
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

private:

    //其实它们配合connId_可以用于给TcpConnection对象起名字
    const std::string name_;
    const std::string ipPort_;
      
    //只是用来给生成的TcpConnection对象起名字的,作为connMap_的key
    int connId_;

    //单纯防止某些用户多次调用start函数
    std::atomic_int started_;



    
    //这个肯定是主线程对应的mainLoop不用想
    
    //只要我真的设置了IO类子线程,那mainLoop中基本只有wakeupfd,listenfd
    //和timerfd三个fd是有可能被注册到epoll监听集合中的
    EventLoop* loop_;
    
    //它封装listenfd,主线程中它就是第一主角
    std::unique_ptr<Acceptor> acceptor_;

    //IO线程的线程池,主线程中它是第二主角
    std::unique_ptr<EventLoopThreadPool> pool_;

    
    //这个哈希表存放着所有TcpConnection的指针
    std::unordered_map<std::string,TcpConnectionPtr> connMap_;



    //这个线程回调有双重含义(但一般都用不着)
    //1.如果我设置了IO子线程数量,那么它代表着subLoop(IO子线程)调用
    //loop函数的事前回调函数
    
    //2.如果我不设置任何IO子线程,就只有一个mainLoop同时负责accept接收连接
    //和tcp连接的各种通信事件,那么threadInitCallback_就代表这些逻辑
    ThreadInitCallback threadInitCallback_;



    //"连接建立"逻辑主要是两部分的逻辑
    //1.TcpServer对象的newConnection函数,这是由TcpServer对象设置给Acceptor
    //对象的.Acceptor底层的AcceptorChannel_在发现全连接队列中有新连接就意味着
    //listenfd关注的读事件就绪,就会先去调用accept函数生成一个connfd,再调用
    //newConnection函数把connfd封装成TcpConnection对象

    //2.就是下面这个数据成员,它是由用户设置给TcpServer对象,再由TcpServer
    //对象设置给TcpConnection对象.当连接建立和关闭都会调用这个回调函数
    //不一定是很重要的功能,可以只是打印日志
    ConnectionCallback connectionCallback_;
    
    
    
    //"消息到达"逻辑的回调函数,由用户直接设置给TcpServer对象再由TcpServer
    //对象设置给TcpConnection对象
    
    //最灵活的回调函数,主要用来完成上层的各种复杂业务
    MessageCallback messageCallback_;
    
    //写完成回调,"消息发送完毕"逻辑的回调函数.由用户设置给TcpServer对象
    //再由TcpServer对象设置给TcpConnection对象.在写完成的时候调用
    WriteCompleteCallback writeCompleteCallback_;
};

