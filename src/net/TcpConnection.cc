#include "TcpConnection.h"
#include "Channel.h"
#include "Socket.h"
#include "Logging.h"
#include "TimerId.h"
#include <unistd.h>


//保证subloop的指针不允许是空指针
static EventLoop* CheckLoopNotNull(EventLoop* loop)
{
    if(!loop)  LOG_FATAL<<"mainLoop is null!";
    return loop;
}



TcpConnection::TcpConnection(EventLoop* loop,
    std::string_view name,int connfd,
    const InetAddress& localAddr,const InetAddress& peerAddr)
:state_(StateE::kConnecting),loop_(CheckLoopNotNull(loop)),name_(name)
,socket_(std::make_unique<Socket>(connfd))
,channel_(std::make_unique<Channel>(connfd,loop_))
,localAddr_(localAddr),peerAddr_(peerAddr)
,highWaterMark_(1024*64*64)
{
    //初始化完上面的数据,就是给Channel对象(对应connfd)设置回调函数
    //不同于其他类型的Channel,connfd对应的Channel对象堪称最复杂,四种
    //回调函数全部都需要设置
    channel_->setReadCallback(
        [this](TimeStamp receiveTime)
        {
            handleRead(receiveTime);
        });
    channel_->setWriteCallback(
        [this]()
        {
            handleWrite();
        });    
    channel_->setCloseCallback(
        [this]()
        {
            handleClose();
        });
    channel_->setErrorCallback(
        [this]()
        {
            handleError();
        });
    LOG_INFO<<"TcpConnection::ctor["<<name_<<"] at fd ="<<connfd;
    socket_->setKeepAlive(true);
}



//TcpConnection对象中是由Socket对象的,因此销毁的时候会连带着它一起销毁
//这样就是真正的关闭connfd
TcpConnection::~TcpConnection()
{
    LOG_INFO<<"TcpConnection::dtor["<<name_<<"] at fd="
        <<channel_->getFd()<<" state="<<static_cast<int>(state_);
}


//间接调用下面的函数
void TcpConnection::send(Buffer* buf)
{
    send(buf->retrieveAllAsString());
}





//这个函数大部分时候都是被用户在connectionCallback_,messageCallback_中调用
//也就是说很有可能在Channel对象的读回调(handleRead)中被调用

//但毕竟是暴露给用户的,谁知道用户会不会自己另外开启一个线程再调用这个sen函数呢
//因此是必须保证线程安全的,即便是其他线程调用也要转交给IO线程去调用.
void TcpConnection::send(std::string_view message)
{
    if(state_==StateE::kConnected)
    {
        loop_->runInLoop(
            [this,str=std::string(message)]()
            {
                sendInLoop(str);
            });
    }
}





//需要注意:在连接断开之前读事件是Channel一直关心的,但是写事件并不能一直关心
//这样会造成busy_loop.具体逻辑需要详细说明

//如果我要发送100B的数据,假如socket的发送缓冲区只能复制到80B的数据,那么剩下的20B
//数据怎么办呢?就这样直接阻塞住吗?肯定不行,我会把剩下20B数据放到outputBuffer_.
//然后再让Channel对象关心写事件并注册.

//由此可见所谓的只要outputBuffer_上还有数据没有发送完,那么就是Channel对象就可写
//可以调用写回调,具体就是把outputBuffer_上的数据也发送出去

//但是之前也说了写事件是不能一直注册着的,为了防止busy_loop需要在outBuffer_上
//的数据全部发送完成时,让Channel对象不再关注写事件
void TcpConnection::sendInLoop(std::string_view message)
{
    size_t wroten=0,remaining=message.size();
    bool faultError=false;
    if(state_==StateE::kDisconnected)
    {
        LOG_ERROR<<"disconnected,give up writing";
        return;
    }

    //这种情况对应Channel对象是第一次发送数据,因为只有一次没有全部把数据发到
    //socket缓冲区的情况下,才有可能导致outputBuffer_上有数据并注册上写事件
    if(!channel_->isWriting()&&outputBuffer_.readableBytes()==0)
    {
        wroten=::write(channel_->getFd(),message.data(),message.size());
        if(wroten>=0)
        {
            remaining-=wroten;

            //这种情况意味着确实把message数据全部发送完了
            //那就调用"消息已发送完成"的回调函数(当然不保证对方已经接收到)
            if(remaining==0&&writeCompleteCallback_)
            {
                loop_->queueInLoop(
                    [this]()
                    {
                        writeCompleteCallback_(shared_from_this());
                    });
            }
        }
        else
        {
            if(errno!=EWOULDBLOCK)
            {
                LOG_ERROR<<"TcpConnection::sendInLoop";
                if(errno==EPIPE||errno==ECONNRESET)  faultError=true;
            }
        }
    }

    //走到这里意味着message数据没有完全拷贝到connfd的socket发送缓冲区上
    //那么剩余的数据全部都放到outputBuffer_上,并给connfd注册写事件
   
    //只要这个TcpConnection(实际上也对应一个connfd的Channel)的outputBuffer_中还
    //有没发完的数据,那么同一个TcpConnection再度sendInLoop时候也是不可能把数据放
    //到socket缓冲区,而是追加到outputBuffer_,这样保证了数据到达的有序性
    if(!faultError&&remaining>0)
    {
        ssize_t oldLen=outputBuffer_.readableBytes();

        //这个情况就说明outputBuffer_中的数据超过了规定的水位量
        //那么就需要调用高水位回调函数了,但是高水位回调只会在第一次超过水
        //位的时候调用一次,不可能反复被调用的.
        if(oldLen+remaining>=highWaterMark_&&
            oldLen<highWaterMark_&&highWaterCallback_)
        {
            loop_->queueInLoop(
                [this,num=oldLen+remaining]()
                {
                    highWaterCallback_(shared_from_this(),num);
                });
        }
        outputBuffer_.append(message.substr(wroten));
        if(!channel_->isWriting())  channel_->enableWriting();
    }
}





//和send逻辑是一样的,大部分时候也是在Channel对象的回调函数中被调用
//也就是说大部分时候不会跨线程调用
void TcpConnection::shutdown()
{
    if(state_==StateE::kConnected)
    {
        setState(StateE::kDisconnecting);
        loop_->runInLoop(
            [this]
            {
                shutdownInLoop();
            });
    }
}





//这个函数用来关闭写端,但是一定要保证缓冲区中没有数据,如果还有数据没有发完
//那是不会关闭写端的!

//关闭写端后这个tcp连接的服务端就不会发送数据只会接收对面的数据了
//一直等待对面也关闭,那么读到的数据量为0,也就会处理"连接断开"的逻辑了
void TcpConnection::shutdownInLoop()
{
    if(!channel_->isWriting())  socket_->shutdownWrite();
}




//强制关闭连接,实际上就是直接调用handleClose
//也是会对外暴露到用户的,因此也需要考虑跨线程调用的情况
void TcpConnection::forceClose()
{
    if(state_==StateE::kConnected||state_==StateE::kDisconnecting)
    {
        setState(StateE::kDisconnecting);

        //handleClose有可能
        loop_->queueInLoop(
            [conn=shared_from_this()]()
            {
                conn->forceCloseInLoop();
            });
    }
}



//其实就是上面函数的基础上使用一下定时器延后调用
void TcpConnection::forceCloseWithDelay(double seconds)
{
    if(state_==StateE::kConnected||state_==StateE::kDisconnecting)
    {
        loop_->runAfter(seconds,[conn=shared_from_this()]()
                                {
                                    conn->forceCloseInLoop();
                                });
    }
}




//强制让Channel对象调用handleClose(正常情况是只有在read对方数据为0才会调用)
void TcpConnection::forceCloseInLoop()
{
    if(state_==StateE::kConnected||state_==StateE::kDisconnecting)
    {
        handleClose();
    }
}




//"连接建立"事件的最后一步
//最重要的当然就是让Channel对象关注可读事件(销毁前永久关注),这样epoll的监听
//集合中就会有connfd和它关注的事件了
void TcpConnection::connectEstablished()
{
    setState(StateE::kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading();

    //这个函数由用户设置再经过TcpServer对象传给当前TcpConnection对象
    //可以只是简单的输出日志
    if(connectionCallback_)  connectionCallback_(shared_from_this());
}





//连接断开过程中TcpConnection对象调用的倒数第二个函数(倒数第一个是析构函数)
//如果是从handleClose中一路走过来(之前是kDisconnecting状态),那这个函数基本
//就是走个流程.

//但也有可能是从kConnected状态直接到这里
void TcpConnection::connectDestroyed()
{
    if(state_==StateE::kConnected)
    {
        setState(StateE::kDisconnected);
        channel_->disableAll();
        if(connectionCallback_)  connectionCallback_(shared_from_this());
    }
}




//Channel对象(封装connfd)的读回调函数,就是把读到的数据从接收缓冲区中取出来

//1.如果读到的数据>0,那么消息已到达,需要针对读到的数据去做消息到达的回调函数,
//执行相应的业务

//2.如果读到的数据==0,说明对端已经关闭,那么服务端也该把这个连接关闭了
//也就是调用handleClose函数(连接关闭的回调函数)

//3.如果读到的数据<0,说明发生了错误,去执行错误回调handleError
void TcpConnection::handleRead(TimeStamp receiveTime)
{
    int saveErrno=0;
    ssize_t n=inputBuffer_.readFd(channel_->getFd(),&saveErrno);
    if(n>0)  
    {
        if(messageCallback_)  
        {
            messageCallback_(shared_from_this(),
                &inputBuffer_,receiveTime);
        }
    }
    else if(n==0)  handleClose();
    else  
    {
        errno=saveErrno;
        LOG_ERROR<<"TcpConnection::handleRead() failed";
        handleError();
    }
}





//Channel对象的写回调函数
//sendInloop那里已经说的很详细了,所谓写回调就是发送outputBuffer_中的残留数据
//直到outputBuffer_中没有数据了,Channel对象才会注销写数据从而避免busy_loop
void TcpConnection::handleWrite()
{
    if(channel_->isWriting())
    {
        int saveErrno=0;

        //把outBuffer_的残留数据取出来发送出去(写到connfd的socket发送缓冲区)
        //但是完全有可能依然没有全部发完
        ssize_t n=outputBuffer_.writeFd(channel_->getFd(),&saveErrno);
        
        if(n>0)
        {
            outputBuffer_.retrieve(n);
            if(outputBuffer_.readableBytes()==0)  
            {
                //走到这里说明outputBuffer_中的数据全部读完了
                //Channel对象可以不去关注写事件了
                channel_->disableWriting();

                //消息已经全部发送完成,可以调用写完成回调函数了
                if(writeCompleteCallback_) 
                {
                    loop_->queueInLoop(
                    [this]()
                    {
                        writeCompleteCallback_(shared_from_this());
                    });
                }
                
                if(state_==StateE::kDisconnecting)  shutdownInLoop();
            }
        }
        else  LOG_ERROR<<"TcpConnection::handleWrite() failed";
    }
    else
    {
        LOG_ERROR<<"TcpConnection fd="
            <<channel_->getFd()<<" is down,no more writing";
    }
}




//连接断开的回调.Channel对象此时不会关注任何事件并被移出epoll的监听集合
//最后还会去调用TcpServer的removeConnection函数
void TcpConnection::handleClose()
{
    setState(StateE::kDisconnected);
    channel_->disableAll();
    TcpConnectionPtr conn{shared_from_this()}; 
    if(connectionCallback_)  connectionCallback_(conn);
    if(closeCallback_)  closeCallback_(conn);
}



//错误处理,相对不是那么重要
void TcpConnection::handleError()
{
    int optval;
    socklen_t optlen=sizeof optval;
    int err=0;
    if(::getsockopt(channel_->getFd(),SOL_SOCKET,SO_ERROR,&optval,&optlen))
    {
        err=errno;
    }
    else  err=optval;
    
    // 忽略常见的网络错误 (Broken pipe, Connection reset by peer)
    // 在高并发压测或客户端强制断开时，这些错误是预期的
    if (err == EPIPE || err == ECONNRESET) {
        LOG_WARN << "TcpConnection::handleError name:" << name_ 
                 << " - SO_ERROR:" << err << " (Connection reset/closed by peer)";
    } else {
        LOG_ERROR << "TcpConnection::handleError name:" << name_ 
                  << " - SO_ERROR:" << err;
    }
}

