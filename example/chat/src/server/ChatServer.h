#pragma once

#include "TcpServer.h"

class ChatServer
{
public:
    ChatServer(EventLoop* loop,const InetAddress& addr
        ,std::string_view name);
    
    void start()
    {
        server_.start();
    }

    void setThreadNum(int num)
    {
        server_.setThreadNum(num);
    }

    static void closeExceptionReset();

private:
    void onConnection(const TcpConnectionPtr& conn);
    void onMessage(const TcpConnectionPtr& conn,Buffer* buffer
        ,TimeStamp timeStamp);
    
private:
    TcpServer server_;
};

