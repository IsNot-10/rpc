#pragma once

#include <sys/socket.h>
#include <netinet/in.h>

namespace SocketAPI
{
    int createNonblocking();
    int getSocketError(int sockfd);
    sockaddr_in getLocalAddr(int sockfd);
    sockaddr_in getPeerAddr(int sockfd);
    bool isSelfConnect(int sockfd);
}