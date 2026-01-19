#pragma once

#include "EventLoop.h"
#include "Callbacks.h"
#include "InetAddress.h"
#include "Buffer.h"
#include <any>

class Socket;


//TcpConnection是muduo中最困难的一个类,不仅生命周期复杂,还需要设计好
//tcp网络编程的三个半事件(连接建立,连接销毁,消息到达和消息发送完毕)

//在网络库轮子中,TcpConnection对象主要由TcpServer和TcpClient拥有,而用户也会
//拥有TcpConnection对象.为了防止用户手动delete掉TcpConnection对象,那就必须用
//std::shared_ptr去管理它的生命周期.

class TcpConnection
:NonCopy,public std::enable_shared_from_this<TcpConnection>
{
private:
    //TcpConnection对象刚刚被创建时,state_绝对是kConnecting
    //而调用connectEstablished函数后,state_会变为kConnected
    
    //调用shutdown后变为kDisConnecting,然后必然面临handleClose将状态设为
    //kDisConnected.而调用forceClose函数也会导致先变为kDisConnecting,然后
    //也会面临handleClose将状态变为kDisConnected.

    //而kConnected直接转变为kDisConnected也是可能的,比如TcpServer析构函数
    enum class StateE
    {   
        kDisconnected, 
        kConnecting,   
        kConnected,    
        kDisconnecting 
    };

public:
    TcpConnection(EventLoop* loop,std::string_view name,int connfd,
            const InetAddress& localAddr,const InetAddress& peerAddr);
    ~TcpConnection();
    void send(std::string_view message);
    void send(Buffer* buf);
    void shutdown();
    void forceClose();
    void forceCloseWithDelay(double seconds);
    void connectEstablished(); 
    void connectDestroyed();  

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

    //这个最特殊,只有TcpServer对象或者TcpClient对象会调用它
    //不可能由用户从最外层调用这个函数
    void setCloseCallback(CloseCallback cb)
    {
        closeCallback_=std::move(cb);
    }

    //设置高水位回调,必定是TcpServer已经启动后调用的
    void setHighWaterCallback(HighWaterCallback cb,size_t highWaterMark)
    {
        highWaterCallback_=std::move(cb);
        highWaterMark_=highWaterMark;
    }

    EventLoop* getLoop()const
    {
        return loop_;
    }

    const std::string& getName()const
    {
        return name_;
    }

    const InetAddress& getLocalAddr()const
    {
        return localAddr_;
    }

    const InetAddress& getPeerAddr()const
    {
        return peerAddr_;
    }

    bool connected()const
    {
        return state_==StateE::kConnected;
    }

    void setContext(const std::any& context)
    {
        context_=context;
    }

    const std::any& getContext()const
    {
        return context_;
    }

    std::any* getMutableContext()
    {
        return &context_;
    }

private:
    void handleRead(TimeStamp receiveTime);
    void handleWrite();
    void handleClose();
    void handleError();
    void sendInLoop(std::string_view message);
    void shutdownInLoop();
    void forceCloseInLoop();
    
    void setState(StateE state)
    {
        state_=state;
    }

private:

    //连接有4种状态,上面的枚举类就说明了
    StateE state_;

    //在设置了IO子线程数量的情况下,这个loop肯定是subLoop(也就是IO子线程对应的)
    EventLoop* loop_;
    
    const std::string name_;

    //对应connfd的Socket对象
    std::unique_ptr<Socket> socket_;
    
    //封装connfd的Channel对象
    std::unique_ptr<Channel> channel_;
    
    //本服务器地址和对端地址
    const InetAddress localAddr_;
    const InetAddress peerAddr_;

    //高水位标准,outputBuffer_中的数据量超过它就会触发高水位回调
    size_t highWaterMark_;

    //连接回调,由用户设置.在连接建立和断开的时候可能都会被调用
    ConnectionCallback connectionCallback_;

    //消息到达的回调,由用户设置.最具灵活性的回调函数,用来处理各种复杂业务
    MessageCallback messageCallback_;
    
    //消息已发送完成的回调,由用户设置.
    WriteCompleteCallback writeCompleteCallback_;
    
    //并非用户提供,仅由TcpServer或TcpClient提供
    //其实就是TcpServer::removeConnection
    CloseCallback closeCallback_;
    
    //用户提供,可以跳过TcpServer对象之间给TcpConnection对象设置
    HighWaterCallback highWaterCallback_;

    //接收缓冲区,用户一般会从上面取出数据
    Buffer inputBuffer_;
    
    //发送缓冲区,用户一般不会直接接触
    Buffer outputBuffer_;

    //这个数据成员提供很强的弹性,它可以根据不同的业务需求设置成不同类型的数据
    std::any context_;
};

