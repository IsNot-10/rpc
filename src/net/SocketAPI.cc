#include "SocketAPI.h"
#include "Logging.h"

namespace SocketAPI
{
    //创建一个非阻塞socket对象
    int createNonblocking()
    {
        int sockfd=::socket(AF_INET
            ,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,IPPROTO_TCP);
        if(sockfd<0)  LOG_FATAL<<"socket create err "<<errno;
        return sockfd;
    }
    
    int getSocketError(int sockfd)
    {
        int optval;
        socklen_t len=static_cast<socklen_t>(sizeof optval);
        if(::getsockopt(sockfd,SOL_SOCKET,SO_ERROR,&optval,&len)<0)
        {
            return errno;
        }
        return optval;
    }

    sockaddr_in getLocalAddr(int sockfd)
    {
        sockaddr_in addr;
        socklen_t len=static_cast<socklen_t>(sizeof addr);
        if(::getsockname(sockfd,(sockaddr*)&addr,&len)<0)
        {
            LOG_FATAL<<"SocketAPI::getLocalAddr";
        }
        return addr;
    }
    
    sockaddr_in getPeerAddr(int sockfd)
    {
        sockaddr_in addr;
        socklen_t len=static_cast<socklen_t>(sizeof addr);
        if(::getpeername(sockfd,(sockaddr*)&addr,&len)<0)
        {
            LOG_FATAL<<"SocketAPI::getPeerAddr";
        }
        return addr;
    }

    bool isSelfConnect(int sockfd)
    {
        sockaddr_in localAddr=getLocalAddr(sockfd);
        sockaddr_in peerAddr=getPeerAddr(sockfd);
        return localAddr.sin_port==peerAddr.sin_port
            &&localAddr.sin_addr.s_addr==peerAddr.sin_addr.s_addr;
    }
}