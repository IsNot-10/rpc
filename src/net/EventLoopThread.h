#pragma once

#include "Thread.h"
#include <mutex>
#include <condition_variable>

class EventLoop;


//这个类代表的是IO线程,而普通线程只需要Thread类就够了
//这里其实体现分层的思想,把EventLoop和Thread一起封装起来
class EventLoopThread
:NonCopy
{
public:
    using ThreadInitCallback=std::function<void(EventLoop*)>;

public:
    EventLoopThread(ThreadInitCallback cb=ThreadInitCallback{}
        ,std::string_view name="");
    ~EventLoopThread();
    EventLoop* startLoop();

private:
    //thread_绑定的线程回调函数就是它
    void ThreadFunc();

private:
    EventLoop* loop_;
    Thread thread_;

    //callback_是在loop前进行的循环前回调,不一定要设置
    ThreadInitCallback callback_;
    std::mutex mtx_;
    std::condition_variable cond_;
};

