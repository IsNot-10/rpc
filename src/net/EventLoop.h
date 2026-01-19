#pragma once

#include "NonCopy.h"
#include "CurrentThread.h"
#include "TimeStamp.h"
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>

class Channel;
class Epoller;
class TimerId;
class TimerQueue;




class EventLoop
:NonCopy
{
public:
    using Functor=std::function<void()>; 

public:
    EventLoop();
    ~EventLoop();

    //EventLoop对象最最核心的函数,这个循环中一直调用epoll_wait
    void loop();

    //退出loop循环
    void quit();

    //执行回调函数,这两个函数需要后面详细说明
    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);

    //这个函数底层直接调用Epoller类的同名函数updateChannel
    //而它们又只会被Channel对象通过调用update函数间接调用
    void updateChannel(Channel* channel);

    //这四个函数就是负责定时器相关的功能
    TimerId runAt(TimeStamp timeStamp,Functor cb);
    TimerId runAfter(double waitTime,Functor cb);
    TimerId runEvery(double interval,Functor cb);
    void cancle(TimerId timerId);

    
    bool isInLoopThread()const
    {
        return CurrentThread::tid()==threadId_;
    }

    TimeStamp pollReturnTime()const 
    { 
        return pollReturnTime_; 
    }


private:

    //这个函数负责唤醒逻辑,让wakeupfd_可读
    //wakeupfd_本身也只关心读事件
    void wakeup();

    //wakeupfd_的读回调函数
    void handleRead(TimeStamp receiveTime);

    //处理pendingFunctors_中的回调任务
    void doPendingFunctor();

private:
    static constexpr int kPollTimeMs=10000;

private:

    //one loop per thread,那么每个EventLoop当然要绑定创建它的IO线程的id
    const pid_t threadId_;

    //判断是否退出事件循环了
    std::atomic_bool quit_;
    
    std::atomic_bool callPending_;
    
    //表示唤醒逻辑.如果某个loop一直卡在epoll_wait(其实就是没有fd就绪的时候)
    //但我又希望这个loop能完成我现在派发给它的任务,那就需要让这个wakeupfd
    //就绪可读.这样这个loop(线程)就会被唤醒去执行任务
    int wakeupfd_;
    std::unique_ptr<Channel> wakeupChannel_;
    
    TimeStamp pollReturnTime_;
    
    //每个事件循环/线程都对应着一个IO多路复用组件,也就是Epoller对象
    std::unique_ptr<Epoller> poller_;
    
    //定时器对象
    std::unique_ptr<TimerQueue> timerQue_;
    
    //表示这一轮epoll_wait返回的channel集合
    std::vector<Channel*> activeChannels_;
    
    //这是唯一需要用互斥锁保证,可能会被多个线程共享的容器
    //它内部存放了各种回调任务,因为除了Channel对象要执行它们的回调函数
    //loop本身也可能需要执行其他的回调逻辑
    std::mutex mtx_;
    std::vector<Functor> pendingFunctors_;
};

