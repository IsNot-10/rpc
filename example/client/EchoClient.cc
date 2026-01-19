#include "EchoClient.h"
#include "Logging.h"

EchoClient::EchoClient(EventLoop* loop
    ,const InetAddress& serverAddr,std::string_view name)
:client_(loop,serverAddr,name)
{
    client_.setNewConnectionCallback(
        [this](const TcpConnectionPtr& conn)
        {
            onConnection(conn);
        });
    client_.setMessageCallback(
        [this](const TcpConnectionPtr& conn
            ,Buffer* buffer,TimeStamp timeStamp)
        {
            onMessage(conn,buffer,timeStamp);
        });
}

void EchoClient::connect()
{
    client_.connect();
}

void EchoClient::onConnection(const TcpConnectionPtr& conn)
{
    LOG_TRACE<<conn->getLocalAddr().getIpPort()<<" -> "
        <<conn->getPeerAddr().getIpPort()<<" is "
        <<(conn->connected()?"UP":"DOWN");
    conn->send("hello,world");
}

void EchoClient::onMessage(const TcpConnectionPtr& conn
    ,Buffer* buffer,TimeStamp timeStamp)
{
    std::string msg=buffer->retrieveAllAsString();
    LOG_TRACE<<conn->getName()<<" recv "<<msg.size()
        <<" bytes at "<<timeStamp.toFormattedString();
    conn->send(msg);
}