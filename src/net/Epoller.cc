#include "Epoller.h"
#include "TimeStamp.h"
#include "Channel.h"
#include "Logging.h"
#include <unistd.h>


//Epoll算是个RAII类,会在构造的时候创建epoll_fd,析构时关闭它
//epoll_fd算是muduo中极少数没有让Channel封装的fd
Epoller::Epoller(EventLoop* loop)
:epollfd_(epoll_create1(EPOLL_CLOEXEC))
,eventList_(kInitEventListSize)
{
    if(epollfd_<0)  LOG_FATAL<<"epoll_create() error:"<<errno;
}


Epoller::~Epoller()
{
    ::close(epollfd_);
}



//最重要的一个函数,实际上底层就是epoll_wait返回就绪事件以及对应Channel对象
//它也会调用fillActiveChannels填充EventLoop对象给它的空容器,把所有
//epoll_wait返回的Channel对象装入这个空容器返还给EventLoop对象即可
TimeStamp Epoller::Poll(int timeoutMs,ChannelList* activeChannels)
{
    int numEvents=::epoll_wait(epollfd_,&*eventList_.begin(),
        static_cast<int>(eventList_.size()),timeoutMs);
    int saveErrno=errno;

    //每一轮epoll_wait中如果确实存在关注事件已经就绪的Channel
    //那就把它装入activeChannels这个容器(这一步需要调用fillActiveChannels函数)
    if(numEvents>0)  
    {
        fillActiveChannels(numEvents,activeChannels);
        if(numEvents==eventList_.size())  eventList_.resize(eventList_.size()*2);
    }
    else if(numEvents==0)  
    {
        LOG_DEBUG<<"timeout!";
    }
    else
    {
        if(saveErrno!=EINTR)
        {
            errno=saveErrno;
            LOG_ERROR<<"Epoller::poll() failed ";
        }
    }
    return TimeStamp::now();
}








//只会在Poll函数中被调用的工具函数
//它会首先根据epoll_wait返回的就绪集中的events去让Channel设置对应的
//revents_(返回的事件),然后填充activeChannels容器给EventLoop对象去使用

//后续过程猜都能猜到,肯定就是让这些Channel对象根据revents_去调用对应回调函数
void Epoller::fillActiveChannels(int numEvents,ChannelList* activeChannels)const
{
    for(int i=0;i<numEvents;i++)
    {
        Channel* channel=static_cast<Channel*>(eventList_[i].data.ptr);
        channel->setRevent(eventList_[i].events);
        activeChannels->emplace_back(channel);
    }
}





//updateChannel在最外层其实是先被Channel对象通过调用update,底层再调用
//EventLoop对象的updateChannel同名函数,再次间接调用的

//这个函数其实就是用来更改Channel本身的状态
//主要是Channel对象底层的fd_有可能会新关注事件或者关心的事件改变
void Epoller::updateChannel(Channel* channel)
{
    //如果channel的fd_以前从来没有注册到epoll红黑树监听集中
    //那么当然是调用epoll_ctl(EPOLL_CTL_ADD...)注册一下这个fd_了
    if(!channel->getState())
    {
        channel->setState(true);
        update(EPOLL_CTL_ADD,channel);
    }
    else 
    {
        //如果channel不再关注任何事件了,那么直接把它从epoll监听集中移除
        
        //因为原muduo的removeChannel函数调用时候也保证Channel对象不关注任何事件
        //所以我这里根本没写removeChannel函数,我觉得它是冗余的,单纯用来debug
        if(channel->isNoneEvent())  
        {
            channel->setState(false);
            update(EPOLL_CTL_DEL,channel);
        }

        //这种情况对应于fd_关注的事件仅仅发生了改变
        else  update(EPOLL_CTL_MOD,channel);
    }
}




//只会在updateChannel中被调用的工具函数,它完全调用epoll_ctl去处理上面的逻辑
//这个函数自身也代表了channel的注册工作(表示自己关注什么事件)
void Epoller::update(int operation,Channel* channel)
{
    epoll_event event;
    event.events=channel->getEvent();
    event.data.ptr=channel;
    if(::epoll_ctl(epollfd_,operation,channel->getFd(),&event)<0)
    {
        if(operation==EPOLL_CTL_DEL)
        {
            LOG_ERROR<<"epoll_ctl() del error:"<<errno;
        }
        else
        {
            LOG_FATAL<<"epoll_ctl add/mod error:"<<errno;
        }
    }
}