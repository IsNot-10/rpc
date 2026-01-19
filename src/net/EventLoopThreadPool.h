#pragma once

#include "NonCopy.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

class EventLoopThread;
class EventLoop;


//这个类代表IO线程的线程池,普通线程池用的是ThreadPool
//直接放在TcpServer的核心组件之一
class EventLoopThreadPool
:NonCopy
{
public:
    using ThreadInitCallback=std::function<void(EventLoop*)>;

public:
    EventLoopThreadPool(EventLoop* loop,std::string_view name);
    ~EventLoopThreadPool();

    //启动线程池(实际上就是启动所有IO线程)
    void start(ThreadInitCallback cb=ThreadInitCallback{});
    EventLoop* getNextLoop();
    std::vector<EventLoop*> getAllLoops();

    void setThreadNum(int threadNum)
    {
        threadNum_=threadNum;
    }
    
    const std::string name()const 
    { 
        return name_; 
    }

private:

    //这个loop_代表的就是主线程(当然也是IO线程)对应的mainLoop
    EventLoop* loop_;
    std::string name_;
    int threadNum_;
    int next_;
    std::vector<std::unique_ptr<EventLoopThread>> pool_;
    std::vector<EventLoop*> loopList_;
};

