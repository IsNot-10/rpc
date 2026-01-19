#include "Socket.h"
#include "InetAddress.h"
#include "Logging.h"
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/tcp.h>


//这里需要注意下,虽然Socket对象确实负责sockfd_的销毁,会在析构函数
//中对它调用::close函数,但是不负责它的创建.
Socket::Socket(int sockfd)
:sockfd_(sockfd)
{}

Socket::~Socket()
{
    ::close(sockfd_);
}


//绑定地址
void Socket::bindAddress(const InetAddress& localAddr)
{
    if(0!=::bind(sockfd_,(sockaddr*)localAddr.getSockAddr(),sizeof(sockaddr_in)))
    {
        LOG_FATAL<<"bind sockfd:"<<sockfd_<<" fail";
    }
}

//监听
void Socket::listen()
{
    if(0!=::listen(sockfd_,1024))
    {
        LOG_FATAL<<"listen sockfd:"<<sockfd_<<" fail";
    }
}


//这里生成connfd,也就是TcpConnection对象对应的fd
int Socket::accept(InetAddress* peerAddr)
{
    sockaddr_in addr;
    socklen_t len=sizeof addr;
    int connfd=::accept4(sockfd_,(sockaddr*)&addr,&len,SOCK_NONBLOCK|SOCK_CLOEXEC);
    if(connfd>=0)  peerAddr->setSockAddr(addr);
    else  LOG_ERROR<<"accept4() failed";
    return connfd;
}


//关闭写端
void Socket::shutdownWrite()
{
    if(::shutdown(sockfd_,SHUT_WR)<0)  LOG_ERROR<<"shutdownWrite error";
}


//不启动Nagle算法
void Socket::setTcpNoDelay(bool on)
{
    int optval=on?1:0;
    ::setsockopt(sockfd_,IPPROTO_TCP,TCP_NODELAY,&optval,sizeof optval); 
}



//设置地址复用,其实就是可以使用处于Time-wait的端口
void Socket::setReuseAddr(bool on)
{
    int optval=on?1:0;
    ::setsockopt(sockfd_,SOL_SOCKET,SO_REUSEADDR,&optval,sizeof optval); 
}


//通过改变内核信息,多个进程可以绑定同一个地址
//通俗的说,就是多个服务的ip+port是一样
void Socket::setReusePort(bool on)
{
    int optval=on?1:0;
    ::setsockopt(sockfd_,SOL_SOCKET,SO_REUSEPORT,&optval,sizeof optval); 
}     


void Socket::setKeepAlive(bool on)
{
    int optval=on?1:0;
    ::setsockopt(sockfd_,SOL_SOCKET,SO_KEEPALIVE,&optval,sizeof optval); 
} 