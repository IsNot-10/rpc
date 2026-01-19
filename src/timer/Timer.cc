#include "Timer.h"

Timer::Timer(TimerCallback cb,TimeStamp when,double interval)
:callback_(std::move(cb)),expiration_(when)
,interval_(interval),repeat_(interval_>0.0)
,sequence_(++numCreated)
{}


//重启定时器,如果是一次型定时器就把到期时间置为0
void Timer::restart(TimeStamp now)
{
    if(repeat_)  expiration_=addTime(now,interval_);
    else  expiration_=TimeStamp{};
}


void Timer::run()
{
    callback_();
}