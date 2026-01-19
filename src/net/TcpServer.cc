#include "TcpServer.h"
#include "EventLoopThreadPool.h"
#include "Acceptor.h"
#include "SocketAPI.h"



static EventLoop* CheckLoopNotNull(EventLoop* loop)
{
    if(!loop)  LOG_FATAL<<"mainLoop is null!";
    return loop;
}




TcpServer::TcpServer(EventLoop* loop,const InetAddress& listenAddr
    ,std::string_view name,Option option)
:name_(name),ipPort_(listenAddr.getIpPort()),connId_(0)
,started_(0),loop_(CheckLoopNotNull(loop))
,acceptor_(std::make_unique<Acceptor>(loop_,listenAddr,option==Option::kReusePort))
,pool_(std::make_unique<EventLoopThreadPool>(loop_,name_))
{
    //newConnection函数本质也是listenfd读回调的一部分
    acceptor_->setNewConnectionCallback(
        [this](int connfd,const InetAddress& peerAddr)
        {
            newConnection(connfd,peerAddr);
        });
}




//TcpServer的析构函数
//就是把哈希表中的Tcp连接智能指针全都引用计数-1
//然后让这些tcp连接对应的IO子线程调用connectDestroyed函数

//注:这个就属于跳过了connfd的handleClose函数直接断开连接的典型例子
TcpServer::~TcpServer()
{
    for(auto& [connName,conn]:connMap_)
    {
        TcpConnectionPtr connPtr{conn};
        conn.reset();
        EventLoop* subLoop=connPtr->getLoop();
        subLoop->queueInLoop(
            [connPtr]()
            {
                connPtr->connectDestroyed();
            }); 
    }
}



//启动TcpServer其实就两件事
//1.启动线程池,每个线程对应一个EventLoop对象并且都在loop循环中
//2.Acceptor调用listen函数,让它关心读事件(是否有新的连接)
void TcpServer::start()
{
    if(started_++==0)
    {
        pool_->start(threadInitCallback_);
        acceptor_->listen();
    }
}



void TcpServer::setThreadNum(int threadNum)
{
    pool_->setThreadNum(threadNum);
}





//这个函数用来把主线程中accept函数生成的connfd封装成一个TcpConnection对象,
//再由这个TcpConnection对象分配给一个IO子线程(subLoop)去完成Tcp编程的三个
//半事件.当然也别忘了给它注册各种回调函数以及把它注册到TcpServer对象的哈希表中
void TcpServer::newConnection(int connfd,const InetAddress& peerAddr)
{
    //给TcpConnection起一个名字
    char buf[128]={0};
    snprintf(buf,sizeof buf,"-%s#%d",ipPort_.c_str(),++connId_);
    const std::string connName{name_+buf};

    LOG_INFO<<"TcpServer::newConnection ["<<name_
        <<"] - new connection ["<<connName<<"] from "
        <<peerAddr.getIpPort();
    
    //获取下一个subLoop(对应IO子线程)
    EventLoop* subloop=pool_->getNextLoop();

    //并初始化新的InerAddress表示对端地址(客户端地址)
    InetAddress localAddr{SocketAPI::getLocalAddr(connfd)};

    //初始化这个TcpConnection对象并返回它的指针
    auto conn=std::make_shared<TcpConnection>(
        subloop,connName,connfd,localAddr,peerAddr);

    
    //把它放入当前TcpServer对象的connMap_哈希表中,千万不要小看这一步
    
    //TcpConnection对象需要防止被用户随手删除,因此设计成用std::shared_ptr
    //管理.而TcpServer的connMap_是绝对不可能被用户使用到的,在这里存放一份
    //TcpConnection对象的std::shared_ptr可以保证这个tcp连接对象永远不会被
    //删除(当然,连接断开的时候removeConnection函数会把它从哈希表中删除)
    connMap_.emplace(conn->getName(),conn);

    //前面三个回调函数connectionCallback_,messageCallback_和writeCompleteCallback_
    //都是由用户先设置给TcpServer对象,再在这里由TcpServer设置给TcpConnection对象
    //分别作为"连接建立和销毁","消息到达"和"消息发送完成"的回调函数
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    //就这个设置特殊,其实removeConnection函数算是connfd处理"连接断开"的一部分
    conn->setCloseCallback(
        [this](const TcpConnectionPtr& connPtr)
        {
            removeConnection(connPtr);
        });


    //由IO子线程的loop循环中目前可能就只有个wakeupfd是在epoll监听集合中的
    //connfd这个时候还没有关注任何事件(所以还没有被注册到epoll监听集合中)
    
    //如果设置了IO子线程数量这里必定是跨线程调用.主线程先通过让子线程中的
    //wakeupfd可读从而让其唤醒,子线程再执行pendingFunctors_中的回调函数
    
    subloop->runInLoop(
        [conn]
        {
            //这个函数也是"连接建立"的最后一步
            conn->connectEstablished();
        });
}




//这个函数一般是在IO子线程(subLoop)中被调用的
//更准确的说是connfd的handleClose函数内部调用(处理连接断开的回调函数)
void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
    //只要设置了IO子线程数量,那么这里必然涉及线程切换
    loop_->runInLoop(
        [this,conn]()
        {
            removeConnectionInLoop(conn);
        });
}




//这个函数一定是在主线程(mainLoop)中被调用的,具体是在mainLoop的
//pendingFunctors_容器中作为回调任务被调用

//具体来说就是把哈希表中存放的TcpConnection对象的共享智能指针删除了
//此时TcpConnection对象命悬一线,它被之前拥有它的IO子线程中
//调用connectDestroyed(),就彻底意味着要被删掉了
void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
    LOG_INFO<<"TcpServer::removeConnectionInLoop ["<<name_
        <<"] - connection "<<conn->getName();
    connMap_.erase(conn->getName());
    EventLoop* subLoop=conn->getLoop();

    //如果设置了子线程,这里必然涉及线程切换
    subLoop->queueInLoop(
        [conn]
        {
            conn->connectDestroyed();
        });
}