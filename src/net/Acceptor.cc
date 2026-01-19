#include "Acceptor.h"
#include "InetAddress.h"
#include "TimeStamp.h"
#include "Logging.h"
#include "SocketAPI.h"
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>




//Acceptor是封装listenfd的RAII类,它会在构造函数中创建listenfd并在
//析构函数中销毁它(会调用Socket的析构函数从而关闭listenfd)
Acceptor::Acceptor(EventLoop* loop,const InetAddress& localAddr,bool reuseport)
:loop_(loop)
,acceptSocket_(SocketAPI::createNonblocking())
,acceptChannel_(acceptSocket_.getFd(),loop_)
,listenning_(false)
,idleFd_(::open("/dev/null",O_RDONLY|O_CLOEXEC))
{
    LOG_DEBUG<<"Acceptor create nonblocking socket,[fd= "
        <<acceptChannel_.getFd()<<"]";

    acceptSocket_.setReusePort(reuseport);
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.bindAddress(localAddr);

    //一如既往的在构造函数中给Channel对象注册一下读回调事件        
    acceptChannel_.setReadCallback(
        [this](TimeStamp receiveTime)
        {
            handleRead(receiveTime);
        });
}


//listenfd不再关注任何事件,并把它从epoll监听集合中移除
Acceptor::~Acceptor()
{
    acceptChannel_.disableAll();
    ::close(idleFd_);
}




//TcpServer对象的start函数中会调用它
//调用listen函数意味着listenfd才会开始判断是否存在就绪可读事件

//它观察Socket对象内部的全连接队列中是否有连接,其实就是判断能不能读
//如果能读,acceptorChannel_就会调用读回调处理了(其实就是accept函数)

//acceptChannel_本身也只关注读事件
void Acceptor::listen()
{
    listenning_=true;
    acceptSocket_.listen();
    acceptChannel_.enableReading();
}




//读事件回调其实本质就是调用accept函数生成一个connfd
//再把connfd封装成一个TcpConnection对象,最后分配一个EventLoop对象接管它
void Acceptor::handleRead(TimeStamp receiveTime)
{
    InetAddress peerAddr;
    int connfd=acceptSocket_.accept(&peerAddr);
    if(connfd>=0)
    {
        //这个回调函数由TcpServer对象为当前Acceptor对象设置
        //用于把connfd封装成TcpConnection对象以及一系列初始化操作
        if(newConnectionCallback_)  
        {
            newConnectionCallback_(connfd,peerAddr);
        }
        else
        {
            LOG_DEBUG<<"no newConnectionCallback() function";
            ::close(connfd);
        }
    }
    else
    {
        LOG_ERROR<<"accept() failed";

        //说明当前进程的fd达到了上限
        if(errno==EMFILE)  
        {
            LOG_INFO<<"sockfd reached limit";

            //接受一下连接立刻关闭,用户通知客户端做出相应的处理
            ::close(idleFd_);
            idleFd_=::accept(connfd,nullptr,nullptr);
            ::close(idleFd_);

            //重新占位
            idleFd_=::open("/dev/null",O_RDONLY|O_CLOEXEC);
        }
    }
}