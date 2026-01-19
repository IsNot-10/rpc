#include "InetAddress.h"

InetAddress::InetAddress(std::string_view ip,uint16_t port)
{
    addr_.sin_family=AF_INET;
    addr_.sin_port=htons(port);
    addr_.sin_addr.s_addr=inet_addr(ip.data());
}

InetAddress::InetAddress(const sockaddr_in& addr)
:addr_(addr)
{}

std::string InetAddress::getIp()const
{
    char buf[1024]={0};
    inet_ntop(AF_INET,&addr_.sin_addr,buf,sizeof buf);
    return buf;
}

uint16_t InetAddress::getPort()const
{
    return ntohs(addr_.sin_port);
}

std::string InetAddress::getIpPort()const
{
    const auto ip=getIp();
    const auto port=std::to_string(getPort());
    return ip+":"+port;
}
