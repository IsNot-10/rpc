#include "TcpClient.h"
#include "Connector.h"
#include "SocketAPI.h"
#include "Logging.h"



TcpClient::TcpClient(EventLoop* loop,const InetAddress& serverAddr
    ,std::string_view name)
:loop_(loop)
,connector_(std::make_unique<Connector>(loop_,serverAddr))
,name_(name),retry_(false),connect_(false),connId_(0)
{
    //newConnection算是try_connfd写回调函数的一部分
    connector_->setNewConnectionCallback(
        [this](int sockfd)
        {
            newConnection(sockfd);
        });
    LOG_INFO<<"TcpClient::TcpClient["<<name_
        <<"] - connector ";
}


TcpClient::~TcpClient()
{
    LOG_INFO<<"TcpClient::~TcpClient["<<name_<<"] - connector ";
    TcpConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lock{mtx_};
        conn=conn_;
    }
    if(!conn)  connector_->stop();
}



void TcpClient::connect()
{
    LOG_INFO<<"TcpClient::connect["<<name_<<"] - connecting to "
        <<connector_->getServerAddr().getIpPort();
    connect_=true;
    connector_->start();
}


//客户端关闭tcp连接的写端(但是仍然可以接收数据)
void TcpClient::disconnect()
{
    connect_=false;
    {
        std::lock_guard<std::mutex> lock{mtx_};
        if(conn_)  conn_->shutdown();
    }
}


void TcpClient::stop()
{
    connect_=false;
    connector_->stop();
}




//这里和TcpServer那里类似,客户端的try_connfd变为connfd后(只是状态变化,数字没变)
//将这个connfd封装为TcpConnection对象
void TcpClient::newConnection(int sockfd)
{
    InetAddress localAddr{SocketAPI::getLocalAddr(sockfd)};
    InetAddress peerAddr{SocketAPI::getPeerAddr(sockfd)};
    char buf[32]={0};
    snprintf(buf,sizeof buf,":%s#%d",peerAddr.getIpPort().c_str(),++connId_);
    std::string connName{name_+buf};

    //生成一条TcpConnection对象,并给它注册各种回调函数
    auto conn=std::make_shared<TcpConnection>(
        loop_,connName,sockfd,localAddr,peerAddr);
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    //这里也和TcpServer那里一样的逻辑,不再赘述
    conn->setCloseCallback(
        [this](const TcpConnectionPtr& connPtr)
        {
            removeConnection(connPtr);
        });
    
    //这里其实对应于TcpServer那里把所有TcpConnection对象的智能指针用哈希表存起来
    {
        std::lock_guard<std::mutex> lock{mtx_};
        conn_=conn;
    }
    conn->connectEstablished();
}



void TcpClient::removeConnection(const TcpConnectionPtr& conn)
{
    //客户端这里不会涉及跨线程调用
    {
        std::lock_guard<std::mutex> lock{mtx_};
        conn_.reset();
    }
    conn->connectDestroyed();

    //支持断开重连并且没有调用stop和disconnect的情况下才会真的重连
    if(retry_&&connect_)  
    {
        LOG_INFO<<"TcpClient::connect["<<name_<<"] - Reconnecting to"
            <<connector_->getServerAddr().getIpPort();
        connector_->restart();
    }
}