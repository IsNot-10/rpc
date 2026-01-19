#pragma once

#include "NonCopy.h"
#include "TimeStamp.h"
#include <functional>
#include <atomic>


//表述单个定时器对象.muduo中有一次性定时器,也有每隔一段时间就会调用的定时器
//Timer用std::shared_ptr管理有点小题大做,但是用std::unique_ptr管理也非常难
//为了提供灵活性,说实话还是用裸指针管理相对轻松...
class Timer
:NonCopy
{
public:
    using TimerCallback=std::function<void()>;

public:
    Timer(TimerCallback cb,TimeStamp when,double interval=0.0);
    void restart(TimeStamp now);
    void run();

    TimeStamp getExpiration()const
    {
        return expiration_;
    }

    bool isRepeat()const
    {
        return repeat_;
    }

    int64_t getSequence()const
    {
        return sequence_;
    }

private:

    //每个定时器在到期时都会调用这个回调函数
    TimerCallback callback_;
    
    //定时器到期的时间点(下一次的超时时刻)
    TimeStamp expiration_;
    
    //定时器的超时间隔(如果是一次性定时器就为0.0)
    double interval_;

    //如果是一次性定时器就为false,否则为true
    bool repeat_;

    //创建定时器A,再删除A创建一个定时器B,完全有可能A和B的地址相同
    //所以区分A和B就必须要有一个全局自增的id号
    const int64_t sequence_;
    static inline std::atomic_int64_t numCreated{0};
};

