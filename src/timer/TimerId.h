#pragma once

#include <stdint.h>

class Timer;


//这是定时器模块中,唯一一个会暴露给用户去使用的类.TimerId是用户调用loop->run...()
//增加某个定时器,最终返回这个定时器(Timer对象)相关的TimerId对象,类似于用户句柄.
//后面如果想要cancle这个增加的定时器也是利用这个TimerId对象去删除.

//为什么要设计成这样呢?因为TimerId对象不管理Timer对象的生命周期,它作为值语义的类
//返回给用户,不容易被误删.而且使用一个全局递增的数字也是有原因的(因为两个Timer对象
//的地址依然有可能一样,因为删除一个对象再分配地址,很有可能分配的是之前已删除对象的地址)

//说实话如果不去实现手动cancle某个定时器的功能,根本就不需要这个类
//上述的loop->run...()返回类型是void就行了
class TimerId
{
    friend class TimerQueue;    

public:
    TimerId(Timer* timer=nullptr,int64_t sequence=0)
    :timer_(timer),sequence_(sequence)
    {}

    bool isVaild()const
    {
        return timer_!=nullptr;
    }

private:
    Timer* timer_;
    int64_t sequence_;  
};

