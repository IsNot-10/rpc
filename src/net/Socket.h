#pragma once

#include "NonCopy.h"

class InetAddress;


//socket封装了socket相关的fd并管理它的生命周期
//muduo中Socket对象管理的fd会是listenfd或者connfd
class Socket
:NonCopy
{
public:
    explicit Socket(int sockfd);
    ~Socket();

    //前面这三个函数只有Acceptor对象用的到
    void bindAddress(const InetAddress& localaddr);
    void listen();
    int accept(InetAddress* peeraddr);

    //TcpConnection对象基本只会用到这个函数(关闭写端但是可以读对方数据)
    void shutdownWrite();


    void setTcpNoDelay(bool on);    
    void setReuseAddr(bool on);     
    void setReusePort(bool on);     
    void setKeepAlive(bool on);     
    
    int getFd()const
    {
        return sockfd_;
    }

private:
    const int sockfd_;
};

