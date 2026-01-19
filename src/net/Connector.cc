#include "Connector.h"
#include "Channel.h"
#include "EventLoop.h"
#include "SocketAPI.h"
#include "Logging.h"
#include <unistd.h>


Connector::Connector(EventLoop* loop,const InetAddress& serverAddr)
:loop_(loop),serverAddr_(serverAddr)
,connect_(false),state_(StateE::kDisconnected)
,retryDelayMs_(kInitRetryDelayMs)
{
    LOG_DEBUG<<"ctor["<<this<<"]";
}



//需要防止Connector对象已经删除但相关定时器(回调函数是一个闭包,内部包含Connector
//对象的指针)在执行回调函数的情况
//所以在一定要在Connector对象的析构函数中删掉定时器
Connector::~Connector()
{
    LOG_DEBUG<<"dtor["<<this<<"]";
    cancelRetryTimer();
}




//由TcpClient对象调用connect间接调用
//而TcpClient对象的connect函数又是暴露给用户的,所以当然要考虑跨线程调用
void Connector::start()
{
    connect_=true;
    loop_->runInLoop(
        [this]()
        {
            startInLoop();
        });
}



void Connector::startInLoop()
{
    if(connect_)  connect();
    else  LOG_DEBUG<<"do not connect";
}




void Connector::connect()
{
    //这里就是用::socket创建一个非阻塞socket对象
    //然后获取服务器的ip:端口号,再调用::connect,是客户端网络编程最经典的步骤
    int sockfd=SocketAPI::createNonblocking();
    auto addr=serverAddr_.getSockAddr();
    int ret=::connect(sockfd,(sockaddr*)addr,sizeof(sockaddr_in));
    
    //根据::connect返回的信息去做不同的操作
    int savedErrno=(ret==0)?0:errno;
    switch(savedErrno)
    {
        case 0:
        case EINPROGRESS:
        case EINTR:

        //这个是最理想情况
        case EISCONN:
            connecting(sockfd);
            break;

        case EAGAIN:
        case EADDRINUSE:
        case EADDRNOTAVAIL:
        case ECONNREFUSED:

        //这意味着重新开始最上面的三部操作
        case ENETUNREACH:
            retry(sockfd);
            break;

        case EACCES:
        case EPERM:
        case EAFNOSUPPORT:
        case EALREADY:
        case EBADF:
        case EFAULT:
        case ENOTSOCK:
            LOG_ERROR
                <<"connect error in Connector::startInLoop "
                <<savedErrno;
            ::close(sockfd);
            break;

        default:
            LOG_ERROR<<"Unexpected error in Connector::startInLoop "
                <<savedErrno;
            ::close(sockfd);
            break;
  }
}




//如果::connect函数成功,那么正常流程就会在TcpClient::start函数的最终流程调用
//这个函数.这时候的客户端sockfd就是try_connfd,需要Channel对象去封装它,开始
//关心写事件并注册到epoll监听集合,再注册写回调和错误处理回调
//实际上::connect返回成功就sockfd就已经可写了,但是可写不意味着客户端和服务端
//一定建立成功了
void Connector::connecting(int sockfd)
{
    setState(StateE::kConnecting);

    //这就是封装try_connfd的Channel对象
    //如果网络连接建立成功,那么它就没用了,客户端fd改用新的Channel对象封装
    //也就是"封装connfd"类型的Channel对象,专门处理tcp连接的那一类
    channel_.reset(new Channel{sockfd,loop_});
    channel_->setWriteCallback(
        [this]()
        {
            handleWrite();
        });
    channel_->setErrorCallback(
        [this]()
        {
            handleError();
        });
    channel_->enableWriting();
}




//封装try_connfd(tcp连接真正建立成功以前的形态)的Channel对象的写回调函数
//这里很重要,即便出现socket可写也不一定意味着连接已经成功建立
//还需要用::getsockopt再次确认一下,而且要处理自连接问题.
void Connector::handleWrite()
{
    if(state_==StateE::kConnecting)
    {
        //这个Channel对象已经没用了,需要新的Channel对象去处理tcp连接
        int sockfd=removeAndResetChannel();
        int err=SocketAPI::getSocketError(sockfd);

        //错误,需要重新创建sockfd并调用::connect
        if(err)
        {
            LOG_WARN<<"Connector::handleWrite - SO_ERROR = "<<err;
            retry(sockfd);
        }

        //处理自连接的情况.同样需要重新创建sockfd并调用::connect
        else if(SocketAPI::isSelfConnect(sockfd))
        {
            LOG_WARN<<"Connector::handleWrite - Self connect";
            retry(sockfd);
        }

        //客户端和服务端建立tcp连接终于成功.
        //这里try_connfd正式变成了connfd(其实只是逻辑状态变了,数字没变)
        else
        {
            setState(StateE::kConnected);
            //这里就是把connfd封装成一个TcpConnection对象
            //和TcpServer那里的处理逻辑是几乎一样的
            if(connect_)  newConnectionCallback_(sockfd);
            else  ::close(sockfd);
        }
    }
}




//错误处理的回调函数
void Connector::handleError()
{
    if(state_==StateE::kConnecting)
    {
        int sockfd=removeAndResetChannel();
        int err=SocketAPI::getSocketError(sockfd);
        LOG_TRACE<<"SO_ERROR = "<<err;
        retry(sockfd);
    }
}




//关闭当前的sockfd,在retryDelayMs_时间以后重新创建sockfd再::connect...
//这里用到了定时器处理上面的逻辑
void Connector::retry(int sockfd)
{
    ::close(sockfd);
    setState(StateE::kDisconnected);
    if(connect_)
    {
        LOG_INFO<<"Connector::retry - Retry connecting to "
            <<serverAddr_.getIpPort()<<" in "
            <<retryDelayMs_<<" milliseconds. ";
        
        //增加的定时器保存好,后面可能又要删除它
        retryTimerId_=loop_->runAfter(retryDelayMs_/1000.0,
                        [this]()
                        {
                            startInLoop();
                        });
        retryDelayMs_=std::min(retryDelayMs_*2,kMaxRetryDelayMs);
    }
    else  LOG_DEBUG<<"do not connect";
}









//把当前的Channel对象从epoll的红黑树监听集合中移除掉,别忘了把Channel对象本身也删除
//最后返回Channel对象的sockfd,有可能在以下情况被调用

//1.客户端和服务端的tcp连接已经建立完成,不再需要这个Channel对象了

//2.Channel对象发现了错误并处理回调,会重新建立sockfd并尝试连接,在此之前肯定
//需要删掉这个没用的Channel对象

//3.stopInLoop中调用
int Connector::removeAndResetChannel()
{
    channel_->disableAll();
    int sockfd=channel_->getFd();

    //这里千万不能直接删除Channel对象,因为这个函数本身也是在Channel对象的写回调
    //函数中被调用的,即便不是跨线程也必须强行放到pengdingFunctors_中被执行
    loop_->queueInLoop(
        [this]()
        {
            resetChannel();
        });
    return sockfd;
}




//真正的删掉掉Channel对象
void Connector::resetChannel()
{
    channel_.reset();
}





//只会在客户端的主线程(mainLoop)中被调用,是永远不可能被跨线程调用的

//并非必要,如果TcpClient对象是支持tcp连接后重连的,才有可能在TcpClient对象
//的removeConnection函数中被调用.
void Connector::restart()
{
    setState(StateE::kDisconnected);
    retryDelayMs_=kInitRetryDelayMs;
    connect_=true;
    startInLoop();
}




//会被TcpClient对象的同名stop函数或者析构函数间接调用,和start一样要考虑跨线程问题
void Connector::stop()
{
    connect_=false;
    loop_->runInLoop(
        [this]()
        {
            stopInLoop();
        });
}




//如果是还没进入tcp连接的状态,那什么都不会做的
//如果当前仍然处于::connect成功但是没有正确建立tcp连接的状态
//会删除掉当前的Channel对象并重连
void Connector::stopInLoop()
{
    if(state_==StateE::kConnecting)
    {
        setState(StateE::kDisconnected);
        int sockfd=removeAndResetChannel();
        cancelRetryTimer();
        retry(sockfd);
    }
}



//注销掉定时器
void Connector::cancelRetryTimer()
{
    if(retryTimerId_.isVaild())
    {
        LOG_DEBUG<<"Canceling retry timer";
        loop_->cancle(retryTimerId_);
        retryTimerId_=TimerId{};
    }
}

