#include "TcpClient.h"

class EchoClient
{
public:
    EchoClient(EventLoop* loop
        ,const InetAddress& serverAddr,std::string_view name);
    void connect();

private:
    void onConnection(const TcpConnectionPtr& conn);
    void onMessage(const TcpConnectionPtr& conn
        ,Buffer* buffer,TimeStamp timeStamp);

private:
    TcpClient client_; 
};