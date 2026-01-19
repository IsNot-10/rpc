#include "Channel.h"
#include "TimeStamp.h"
#include "EventLoop.h"
#include "Logging.h"

Channel::Channel(int fd,EventLoop* loop)
:fd_(fd),loop_(loop),events_(0),revent_(0)
,state_(false),tied_(false)
{}




//这个函数底层就是让当前Channel对象所属的EventLoop对象调用updateChannel
//函数完成(而EventLoop::updateChannel底层调用的Epoller对象的updateChannel)

//这个函数可以保证Channel对象(包括fd_以及它关心的事件一起)被注册到epoll的监听集
void Channel::update()
{
    loop_->updateChannel(this);
}



//只有封装connfd的Channel对象才用的到这个函数,表示连接建立以及完成
//它绑定了TcpConnection的弱引用智能指针,不仅仅是为了避免循环引用,更是为了保证
//Channel对象处理一系列回调函数的期间,TcpConnection对象不会析构,否则就会导致
//Channel对象执行了一个回调函数就被析构,执行下一个回调函数导致空悬指针错误.
void Channel::tie(const std::shared_ptr<TcpConnection>& conn)
{
    tied_=true;
    conn_=conn;
}




void Channel::handleEvent(TimeStamp receiveTime)
{
    //封装connfd的Channel对象才会走进这个判断分支
    //这么设计可以防止Channel对象处理handleEvent期间,自己被析构.
    //那么问题来了,为什么会有这种情况出现?继续往下看.

    if(tied_)
    {
        //我认为这里生成的强引用智能指针是不可能空的.因为提升失败证明TcpConnection
        //对象已经被析构了,Channel对象由于被前者管理生命周期也就会被顺带着析构,那
        //我居然还能走进这个函数?epoll_wait难道给我返回了一个空悬的Channel指针?
        //这样的话就是设计上的严重问题.

        //我这里生成强智能指针,根本目的是为了保证handleEventWithGuard对象会畅通
        //无阻的运行,不可能出现执行了一半TcpConnection对象被析构进而导致Channel
        //对象连带着被析构的问题(具体请看handleEventWithGuard)函数逻辑
        auto conn=conn_.lock();
        handleEventWithGuard(receiveTime);
    }

    //如果是其他类型的Channel对象,根本就不需要考虑上面的逻辑直接调用回调函数
    else  handleEventWithGuard(receiveTime);
}




//真正的处理回调函数逻辑,这个函数绝对安全,不可能处理一半的时候当前Channel对象被析构
//根据epoll_wait返回的就绪集得到的revent_去调用对应的回调函数
void Channel::handleEventWithGuard(TimeStamp receiveTime)
{
    //对于封装connfd的Channel对象,closeCallback_对应的实际上是TcpConnection对象
    //的handleClose函数,那个函数最后就是会把TcpConnection对象析构掉的.然而我在
    //上面的handleEvent函数中已经给TcpConnection对象引用计数+1,因此不会析构.
    //但是如果我没有做那样的操作,只要走进了这个分支Channel对象会在TcpConnection
    //对象的析构函数中被销毁

    //但是函数本身不可能知道当前Channel对象已经被销毁,肯定是会继续往下走的
    //那么肯定就是未定义行为了,因为在访问已经被释放的内存了!
    if((revent_&EPOLLHUP)&&!(revent_&EPOLLIN))
    {
        if(closeCallback_)  closeCallback_();
    }

    if(revent_&EPOLLERR)
    {
        LOG_WARN<<"the fd="<<fd_<<" - Channel.cc:84 (EPOLLERR)";
        if(errorCallback_)  errorCallback_();
    }
    if(revent_&(EPOLLIN|EPOLLPRI))  
    {
        LOG_DEBUG<<"channel have read events,the fd="<<getFd();
        if(readCallback_)  
        {
            LOG_DEBUG<<"channel call the readCallback_(),the fd="<<getFd();
            readCallback_(receiveTime);
        }
    }
    if(revent_&EPOLLOUT)  
    {
        if(writeCallback_)  writeCallback_();
    }     
}