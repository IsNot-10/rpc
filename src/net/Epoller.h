#pragma once

#include "NonCopy.h"
#include <vector>
#include <sys/epoll.h>

class Channel;
class EventLoop;
class TimeStamp;



//epoll编程中需要用到的数据结构以及重要函数

/*
typedef union epoll_data 
{
    void* ptr;   (另外三个数据成员省略了,因为这里用不到)
}epoll_data_t;

struct epoll_event 
{
    uint32_t events;    //Epoll events 
    epoll_data_t data;  // User data variable 
};

int epoll_ctl(int epfd,int op,int fd,struct epoll_event* event);

int epoll_wait(int epfd,struct epoll_event* events,
    int maxevents,int timeout);
*/

//正常编程都会在epoll_ctl注册fd和它感兴趣事件的时候,把epoll_event对象
//中的events注册成感兴趣的事件(对应Channel的events_),然后epoll_wait
//返回时对应epoll_event对象中的events就是返回的就绪事件(对应Channel中的
//revent_),Channel对象也根据这个revent_去调用对应的回调函数

//muduo中编程逻辑中,epoll_event对象中的data联合体选用了ptr这个数据成员
//而它存放的是Channel地址.这样可以在epoll_wait返回后得到这个Channel





//这里没有像陈硕老师那样提供poll和epoll两种不同的IO多路复用模型,只有epoll
//Epoller类负责的就是muduo中IO多路复用的模块
class Epoller
:NonCopy
{
public:
    using ChannelList=std::vector<Channel*>;

public:
    Epoller(EventLoop* loop);
    ~Epoller();

    //最重要的一个函数,实际上底层就是epoll_wait返回就绪事件以及对应Channel对象
    TimeStamp Poll(int timeoutMs,ChannelList* activeChannels);

    //这个函数表达的是Channel对象关心某个事件在epoll监听集合中状态改变的逻辑
    //实际上会根据不同情况去调用epoll_ctl函数(由update完成)
    void updateChannel(Channel* channel);

private:
    void fillActiveChannels(int numEvents,ChannelList* activeChannels)const;
    
    //基本就是完全调用epoll_ctl
    void update(int operation,Channel* channel);

private:
    static constexpr int kInitEventListSize=16; 

private:
    //每个事件循环(loop)中一个epoll
    int epollfd_;

    //用来存放每轮epoll_wait中有哪些Channel,它们关注的事件到来了
    std::vector<epoll_event> eventList_;
};

