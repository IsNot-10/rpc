#include "ChatServer.h"
#include "ChatService.h"



ChatServer::ChatServer(EventLoop* loop,const InetAddress& addr
    ,std::string_view name)
:server_(loop,addr,name)
{
    server_.setConnectionCallback(
        [this](const TcpConnectionPtr& conn)
        {
            onConnection(conn);
        });
    server_.setMessageCallback(
        [this](const TcpConnectionPtr& conn,Buffer* buffer
            ,TimeStamp timeStamp)
        {
            onMessage(conn,buffer,timeStamp);
        });
}



void ChatServer::onConnection(const TcpConnectionPtr& conn)
{
    if(!conn->connected())  
    {
        ChatService::getInstance().clientCloseReset(conn);
        conn->shutdown();
    }
}



//这里实现了网络和业务模块的解耦合.
//网络模块不必根据不同的消息类型做不同的回调函数,这个任务交给业务模块处理
void ChatServer::onMessage(const TcpConnectionPtr& conn,Buffer* buffer
    ,TimeStamp timeStamp)
{
    std::string msg=buffer->retrieveAllAsString();
    auto js=nlohmann::json::parse(msg);
    auto handler=ChatService::getInstance().
            getHandler(js["msgid"].get<int>());
    handler(conn,js,timeStamp);
}


void ChatServer::closeExceptionReset()
{
    ChatService::getInstance().serverCloseExceptionReset();
}