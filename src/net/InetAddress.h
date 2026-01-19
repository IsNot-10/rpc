#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>

//代表tcp通信地址(包含ip地址和port端口号的信息)
//这个类是值语义的
class InetAddress
{
public:
    explicit InetAddress(std::string_view ip="127.0.0.1",uint16_t port=8000);
    explicit InetAddress(const sockaddr_in& addr);
    std::string getIp()const;
    uint16_t getPort()const;
    std::string getIpPort()const;

    const sockaddr_in* getSockAddr()const 
    { 
        return &addr_; 
    }

    void setSockAddr(const sockaddr_in& addr) 
    { 
        addr_=addr; 
    }    

private:
    sockaddr_in addr_;  
};

