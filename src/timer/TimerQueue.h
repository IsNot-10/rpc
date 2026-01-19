#pragma once

#include "NonCopy.h"
#include "TimeStamp.h"
#include "Channel.h"
#include <vector>
#include <set>

class EventLoop;
class Timer;
class TimerId;


//定时器很明显也是需要epoll来管理的,每当存在定时器超时就要处理回调函数
//但是很明显定时器有太多个,怎么处理比较好呢?

//可以考虑把所有定时器按超时时间排序形成一个定时器列表,只要排在最前面的定时器
//(也就是超时时间最早的定时器)超时了,那么整体的定时器列表就可读,处理它的读
//回调函数(实际上就是定时器回调函数)

//那么怎么设计这个定时器列表呢?既然要有序,那可以考虑堆和红黑树
//可是为什么muduo最终选用的是红黑树呢?因为确实存在把不超时的具体某一个定时器
//从定时器列表中删除出去的逻辑,而堆不能做到删掉中间的节点

//因此muduo中还是采用了红黑树管理所有定时器.到期时间作为排序的标准,为了区分相同时
//间到期的两个不同Timer,就用了std::pair<TimerStamp,Timer*>作为红黑树的节点

class TimerQueue
:NonCopy
{
    using Entry=std::pair<TimeStamp,Timer*>;
    using ActiveTimer=std::pair<Timer*,int64_t>;
    using TimerCallback=std::function<void()>;

public:
    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    //在红黑树定时器表timerList_中增加一个定时器
    TimerId addTimer(TimerCallback cb,TimeStamp when,double interval);

    //取消某一个定时器,这才是真正的难点
    void cancle(TimerId timerId);

private:
    void addTimerInLoop(Timer* timer);
    void handleRead();
    bool insert(Timer* timer);
    void reset(const std::vector<Entry>& expired,TimeStamp now);
    std::vector<Entry> getExpired(TimeStamp now);
    void resetTimerfd(TimeStamp expiration);
    void cancleInLoop(TimerId timerId);

private:
    EventLoop* loop_;
    const int timerfd_;
    Channel timerChannel_;

    //这就是红黑树定时器列表
    std::set<Entry> timerList_;

    //如果我不考虑手动删除某个定时器,那完全只需要上面几个数据成员就够了
    //下面的数据成员全是为了手动删除定时器

    //实际上它内部的数量和Timer对象指针和timerList_一样
    //只不过key是TimerId用来手动删除
    std::set<ActiveTimer> activeTimerList_;
    
    
    //下面两个数据成员专门处理这种可能:定时器的超时回调函数调用cancle
    //也就是一个定时器会把自己或者其他定时器注销的情况

    //表示是否正在调用过期函数的回调函数
    //实际上就是用来检测cancle函数是不是在Chanel对象的读回调(handleRead)中被调用的
    bool callingExpired_;

    //即将删除的定时器,为什么要这么设计看后面的分析
    std::set<ActiveTimer> cancleTimerList_;
};

